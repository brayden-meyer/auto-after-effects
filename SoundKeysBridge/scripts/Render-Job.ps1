[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ResultFile,
    [Parameter(Mandatory=$true)][string]$Aerender,
    [Parameter(Mandatory=$true)][string]$Composition,
    [Parameter(Mandatory=$true)][string]$OutputMovie
)
$ErrorActionPreference = 'Stop'
$r = Get-Content -LiteralPath $ResultFile -Raw -Encoding UTF8 | ConvertFrom-Json
if ($r.protocol -ne 1 -or $r.status -ne 'succeeded') {
    throw 'Job did not succeed.'
}
if (!(Test-Path -LiteralPath $r.output_project -PathType Leaf)) { throw 'Prepared project is missing.' }
if (Test-Path -LiteralPath $OutputMovie) { throw 'Refusing to overwrite output movie.' }
# Uses the template's render/output-module settings. -reuse is intentionally
# absent: the preparation process must not be reused as a render worker.
& $Aerender '-project' $r.output_project '-comp' $Composition '-output' $OutputMovie
if ($LASTEXITCODE -ne 0) { throw "aerender failed with exit code $LASTEXITCODE" }
if (!(Test-Path -LiteralPath $OutputMovie -PathType Leaf)) { throw 'Expected movie was not created.' }
Get-Item -LiteralPath $OutputMovie
