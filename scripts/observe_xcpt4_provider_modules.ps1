param([Parameter(Mandatory)] [string] $Image)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Image).Path
$original = Get-Content -Raw xcpt4.json | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant() -ne $original.sha256) {
    throw 'The fixture differs from the validated original run'
}

$provider = 'Microsoft-Windows-Kernel-Process'
$schema = Get-WinEvent -ListProvider $provider
$keyword = @($schema.Keywords | Where-Object Name -eq 'WINEVENT_KEYWORD_IMAGE')
if ($keyword.Count -ne 1) { throw 'Expected the image keyword in the native ETW provider manifest' }
$flags = '0x{0:x}' -f $keyword[0].Value
$session = 'NeverD-xcpt4-' + [Guid]::NewGuid().ToString('N')
$etl = Join-Path $env:RUNNER_TEMP ($session + '.etl')

try {
    & logman create trace $session -o $etl -p $provider $flags 5 -ets
    if ($LASTEXITCODE -ne 0) { throw 'Could not start image-load tracing' }
    try {
        $info = [Diagnostics.ProcessStartInfo]::new($exe)
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $started = [DateTime]::UtcNow
        $child = [Diagnostics.Process]::Start($info)
        try {
            $outTask = $child.StandardOutput.ReadToEndAsync()
            $errTask = $child.StandardError.ReadToEndAsync()
            if (-not $child.WaitForExit(120000)) {
                $child.Kill()
                throw 'xcpt4 timed out during the module observation'
            }
            $finished = [DateTime]::UtcNow
            $programId = $child.Id
            $stdout = $outTask.GetAwaiter().GetResult().Replace("`r`n", "`n")
            $stderr = $errTask.GetAwaiter().GetResult().Replace("`r`n", "`n")
            if ($child.ExitCode -ne $original.exit_code -or $stdout -cne $original.stdout -or $stderr -cne $original.stderr) {
                throw 'The observed original run differs from its separately validated output'
            }
        }
        finally { $child.Dispose() }
    }
    finally {
        & logman stop $session -ets
        if ($LASTEXITCODE -ne 0) { throw 'Could not flush the image-load events' }
    }

    # Resolve native device names emitted by ETW to the corresponding file on this runner.
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using System.Text;
public static class Xcpt4Volumes {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint QueryDosDevice(string name, StringBuilder target, int size);
}
'@
    $drives = @{}
    foreach ($disk in [IO.DriveInfo]::GetDrives()) {
        $drive = $disk.Name.TrimEnd('\')
        $buffer = [Text.StringBuilder]::new(32768)
        if ([Xcpt4Volumes]::QueryDosDevice($drive, $buffer, $buffer.Capacity)) {
            $drives[$buffer.ToString().Split([char]0)[0]] = $drive
        }
    }
    function Resolve-ImagePath([string] $name) {
        if ($name -like '\SystemRoot\*') { return (Join-Path $env:SystemRoot $name.Substring(12)) }
        foreach ($device in $drives.Keys) {
            if ($name.StartsWith($device + '\', [StringComparison]::OrdinalIgnoreCase)) {
                return $drives[$device] + $name.Substring($device.Length)
            }
        }
        return $name
    }

    $wanted = @([IO.Path]::GetFileName($exe), 'ucrtbase.dll', 'vcruntime140.dll')
    $images = @{}
    foreach ($event in Get-WinEvent -Path $etl -Oldest) {
        if ($event.ProviderName -ne $provider -or $event.Id -ne 5) { continue }
        $time = $event.TimeCreated.ToUniversalTime()
        if ($time -lt $started -or $time -gt $finished) { continue }
        $fields = @{}
        foreach ($field in ([xml]$event.ToXml()).Event.EventData.Data) {
            $fields[$field.Name] = $field.'#text'
        }
        if ([uint32]$fields.ProcessID -ne $programId) { continue }
        $name = [IO.Path]::GetFileName($fields.ImageName)
        if ($name -notin $wanted) { continue }
        $path = Resolve-ImagePath $fields.ImageName
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        $stream = [Reflection.PortableExecutable.PEReader]::new([IO.File]::OpenRead($file.FullName))
        try {
            $headers = $stream.PEHeaders
            if ($headers.CoffHeader.Machine -ne 'I386' -or $headers.PEHeader.Magic -ne 'PE32') {
                throw "Expected an x86 PE in $path"
            }
            $pdbs = @(foreach ($entry in $stream.ReadDebugDirectory()) {
                if ($entry.Type -eq 'CodeView') {
                    $pdb = $stream.ReadCodeViewDebugDirectoryData($entry)
                    [ordered]@{ name = [IO.Path]::GetFileName($pdb.Path); guid = $pdb.Guid.ToString('N'); age = $pdb.Age }
                }
            })
            $identity = [ordered]@{ coff_timestamp = $headers.CoffHeader.TimeDateStamp.ToString('X8')
                size_of_image = $headers.PEHeader.SizeOfImage; codeview = $pdbs }
        }
        finally { $stream.Dispose() }
        if ($images.ContainsKey($name) -and $images[$name].sha256 -ne $hash) {
            throw "Two different images were reported for $name"
        }
        $images[$name] = [ordered]@{ file = $file.FullName; sha256 = $hash
            version = $file.VersionInfo.FileVersion; machine = 'i386'; loaded_utc = $time.ToString('o'); pe = $identity }
    }
    foreach ($name in $wanted) {
        if (-not $images.ContainsKey($name)) { throw "Missing a loader event for $name" }
    }
    if ($images[$wanted[0]].sha256 -ne $original.sha256) { throw 'The ETW image does not match the original fixture' }
    [ordered]@{ provider = $provider; event_id = 5; image_sha256 = $original.sha256
        started_utc = $started.ToString('o'); ended_utc = $finished.ToString('o'); pid = $programId
        cases = $original.cases; modules = $images; os_version = [Environment]::OSVersion.Version.ToString()
    } | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 xcpt4-modules.json
}
finally { if (Test-Path -LiteralPath $etl) { Remove-Item -LiteralPath $etl } }
