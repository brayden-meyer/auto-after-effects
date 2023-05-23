#include "AEHost.hpp"
#include "WindowsIO.hpp"

#include <memory>
#include <algorithm>

namespace skb {

class Worker {
public:
    Worker(SPBasicSuite* basic, AEGP_PluginID id, const std::wstring& queue)
        : basic_(basic),
          id_(id),
          queue_(win::canonical(queue)) {
        const DWORD attributes = GetFileAttributesW(queue_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            throw Error("missing_queue", "Queue directory must already exist");
        }

        lock_.reset(new win::Handle(CreateFileW(
            win::join(queue_, L"worker.lock").c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)));
        if (!lock_->valid()) {
            throw Error(
                "worker_locked",
                "Another process owns this queue or the queue is inaccessible");
        }

        // Unknown interrupted effects are never re-run implicitly.
        if (!win::list(queue_, L"*.running").empty()) {
            halted_ = true;
            win::log(queue_, "Found interrupted .running job; halted, no automatic retry");
        }

        win::log(
            queue_,
            "loaded v1.0.0; AfterFX file version="
                + win::fileVersion(win::executable()));
    }

    void idle() noexcept {
        if (busy_ || halted_) {
            return;
        }

        const auto now = GetTickCount64();
        if (now - lastPoll_ < 250) {
            return;
        }
        lastPoll_ = now;
        busy_ = true;

        try {
            const auto jobs = win::list(queue_, L"*.request");
            if (!jobs.empty()) {
                process(jobs.front());
            }
        } catch (const std::exception& error) {
            halted_ = true;
            win::log(queue_, std::string("Worker halted: ") + error.what());
        } catch (...) {
            halted_ = true;
            win::log(queue_, "Worker halted: unknown exception");
        }

        busy_ = false;
    }

private:
    void process(const std::wstring& filename) {
        const auto stem = filename.substr(0, filename.size() - 8);  // .request
        const auto jobId = win::utf8(stem);
        if (!validJobId(jobId)) {
            throw Error("bad_job_filename", "Invalid request basename");
        }

        const auto base = win::join(queue_, stem);
        if (win::exists(base + L".result.json")
            || win::exists(base + L".done")
            || win::exists(base + L".failed")) {
            throw Error(
                "duplicate_job",
                "Job ID already has terminal artifacts; IDs are single-use");
        }

        win::moveNew(base + L".request", base + L".running");

        std::string stage = "claimed";
        bool dispatched = false;
        const auto started = GetTickCount64();

        try {
            const auto job = parseJob(win::read(base + L".running"));
            if (job.id != jobId) {
                throw Error("bad_job_id", "job_id differs from filename");
            }

            const auto profilePath = win::canonical(win::wide(job.profile));
            const auto profileText = win::read(profilePath);
            const auto profile = parseProfile(profileText);
            // Copy the exact profile used into the per-job audit record.
            win::atomicWrite(base + L".profile.txt", profileText);

            auto report = [&](const std::string& next) {
                if (next.compare(0, 10, "inventory:") == 0) {
                    win::atomicWrite(base + L".inventory.json", next.substr(10));
                    return;
                }

                stage = next;
                if (stage == "dispatching") {
                    dispatched = true;
                }

                win::log(queue_, jobId + " " + stage);
                win::atomicWrite(
                    base + L"." + win::wide(stage),
                    "{\"job_id\":" + jsonString(jobId)
                        + ",\"phase\":" + jsonString(stage) + "}\n");
            };

            const auto result = applySoundKeys(basic_, id_, job, profile, report);
            win::atomicWrite(base + L".keys.json", result.keysJson);

            const auto elapsedMs = GetTickCount64() - started;
            const std::string summary =
                "{\"protocol\":1,\"job_id\":" + jsonString(jobId)
                + ",\"status\":\"succeeded\",\"phase\":\"saved\",\"output_project\":"
                + jsonString(job.output)
                + ",\"profile_id\":" + jsonString(profile.id)
                + ",\"route\":" + jsonString(result.route)
                + ",\"elapsed_ms\":" + std::to_string(elapsedMs) + "}\n";
            win::atomicWrite(base + L".result.json", summary);
        } catch (const Error& error) {
            recordFailure(base, jobId, stage, dispatched, error.code, error.what());
            return;
        } catch (const std::exception& error) {
            recordFailure(base, jobId, stage, dispatched, "cpp_exception", error.what());
            return;
        } catch (...) {
            recordFailure(
                base,
                jobId,
                stage,
                dispatched,
                "unknown_exception",
                "Unknown C++ exception");
            return;
        }

        // Once a success result exists it is immutable. An archive rename error
        // must not replace success with failure or retry Sound Keys.
        try {
            win::moveNew(base + L".running", base + L".done");
        } catch (...) {
            halted_ = true;
            win::log(
                queue_,
                jobId + " succeeded, but archive rename failed; worker halted");
        }
    }

    void recordFailure(
        const std::wstring& base,
        const std::string& job,
        const std::string& stage,
        bool dispatched,
        const std::string& code,
        const std::string& message) {
        // After any attempted dispatch, the open project may contain partial
        // changes. Do not process further jobs in that AE process.
        if (dispatched) {
            halted_ = true;
        }

        win::log(queue_, job + " failed " + code + " " + message);
        win::atomicWrite(
            base + L".result.json",
            "{\"protocol\":1,\"job_id\":" + jsonString(job)
                + ",\"status\":\"failed\",\"phase\":" + jsonString(stage)
                + ",\"code\":" + jsonString(code)
                + ",\"message\":" + jsonString(message)
                + ",\"project_may_be_modified\":" + (dispatched ? "true" : "false")
                + ",\"worker_halted\":" + (halted_ ? "true" : "false") + "}\n");
        win::moveNew(base + L".running", base + L".failed");
    }

    SPBasicSuite* basic_;
    AEGP_PluginID id_;
    std::wstring queue_;
    std::unique_ptr<win::Handle> lock_;
    bool busy_ = false;
    bool halted_ = false;
    ULONGLONG lastPoll_ = 0;
};

static std::unique_ptr<Worker> worker;

static A_Err idleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* maxSleep) {
    if (maxSleep) {
        *maxSleep = std::min<A_long>(*maxSleep, 15);  // units are 1/60 sec
    }
    if (worker) {
        worker->idle();
    }
    return A_Err_NONE;
}

static A_Err deathHook(AEGP_GlobalRefcon, AEGP_DeathRefcon) {
    worker.reset();
    return A_Err_NONE;
}

}  // namespace skb

extern "C" __declspec(dllexport) A_Err EntryPointFunc(
    SPBasicSuite* basic,
    A_long driverMajor,
    A_long driverMinor,
    AEGP_PluginID id,
    AEGP_GlobalRefcon* globalRefcon) {
    // AEGP driver versions are not AE marketing versions.
    (void)driverMajor;
    (void)driverMinor;

    if (globalRefcon) {
        *globalRefcon = nullptr;
    }

    std::wstring queue;
    const AEGP_RegisterSuite5* registration = nullptr;

    try {
        const auto executablePath = skb::win::executable();
        const auto executableName = executablePath.substr(executablePath.find_last_of(L"\\/") + 1);
        if (_wcsicmp(executableName.c_str(), L"AfterFX.exe") != 0) {
            return A_Err_NONE;
        }

        queue = skb::win::environment(L"SOUNDKEYS_BRIDGE_QUEUE");
        if (queue.empty()) {
            return A_Err_NONE;  // inert unless configured before AE starts
        }

        skb::worker.reset(new skb::Worker(basic, id, queue));

        const void* rawSuite = nullptr;
        if (basic->AcquireSuite(kAEGPRegisterSuite, kAEGPRegisterSuiteVersion5, &rawSuite)
            || !rawSuite) {
            throw skb::Error("missing_suite", "Cannot acquire AEGP Register Suite5");
        }
        registration = static_cast<const AEGP_RegisterSuite5*>(rawSuite);

        const auto deathErr = registration->AEGP_RegisterDeathHook(id, skb::deathHook, nullptr);
        if (deathErr) {
            throw skb::Error("registration_failed", "Cannot register death hook");
        }

        const auto idleErr = registration->AEGP_RegisterIdleHook(id, skb::idleHook, nullptr);
        basic->ReleaseSuite(kAEGPRegisterSuite, kAEGPRegisterSuiteVersion5);
        registration = nullptr;
        if (idleErr) {
            skb::win::log(queue, "Cannot register idle hook; bridge disabled");
            skb::worker.reset();
        }
        return A_Err_NONE;
    } catch (const std::exception& error) {
        skb::win::log(queue, std::string("Startup failed: ") + error.what());
    } catch (...) {
        skb::win::log(queue, "Startup failed: unknown exception");
    }

    if (registration) {
        basic->ReleaseSuite(kAEGPRegisterSuite, kAEGPRegisterSuiteVersion5);
    }
    skb::worker.reset();
    // Do not display an unattended modal dialog; report startup failure via log.
    return A_Err_NONE;
}
