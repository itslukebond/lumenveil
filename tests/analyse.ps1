# Checks a script against Windows PowerShell 5.1 with PSScriptAnalyzer's compatibility rules.
# Usage: pwsh -NoProfile -File tests/analyse.ps1 <script.ps1>   Exit 0: nothing found, 2: findings, 1: error.
param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'
Import-Module PSScriptAnalyzer
$profile51 = 'win-48_x64_10.0.17763.0_5.1.17763.316_x64_4.0.30319.42000_framework'
$settings = @{
    IncludeRules = @('PSUseCompatibleSyntax', 'PSUseCompatibleCommands', 'PSUseCompatibleTypes')
    Rules        = @{
        PSUseCompatibleSyntax   = @{ Enable = $true; TargetVersions = @('5.1') }
        PSUseCompatibleCommands = @{ Enable = $true; TargetProfiles = @($profile51) }
        PSUseCompatibleTypes    = @{ Enable = $true; TargetProfiles = @($profile51) }
    }
}
$found = @(Invoke-ScriptAnalyzer -Path $Path -Settings $settings)
foreach ($f in $found) { Write-Output ('{0}:{1} {2} {3}' -f $f.ScriptName, $f.Line, $f.RuleName, $f.Message) }
Write-Output ('{0} finding(s)' -f $found.Count)
if ($found.Count -gt 0) { exit 2 }
exit 0
