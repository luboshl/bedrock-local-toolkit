param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Capture', 'Compare', 'Describe')]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [int]$ProcessId,
    [string]$BeforePath,
    [string]$AfterPath,
    [string]$CandidateValuesCsv,
    [string]$PatternValuesCsv,
    [string]$AddressesCsv,
    [float]$ExpectedBefore = [float]::NaN,
    [float]$ExpectedAfter = [float]::NaN
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BeforePath) { $BeforePath = Join-Path $repoRoot 'artifacts\fov-before.json' }
if (-not $AfterPath) { $AfterPath = Join-Path $repoRoot 'artifacts\fov-after.json' }
$CandidateValues = @()
if ($CandidateValuesCsv) {
    $CandidateValues = @($CandidateValuesCsv.Split(',') | ForEach-Object {
        [float]::Parse($_, [Globalization.CultureInfo]::InvariantCulture)
    })
}
$PatternValues = @()
if ($PatternValuesCsv) {
    $PatternValues = @($PatternValuesCsv.Split(',') | ForEach-Object {
        [float]::Parse($_, [Globalization.CultureInfo]::InvariantCulture)
    })
}
$expectedImageSuffix = 'Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe\Minecraft.Windows.exe'
$game = Get-Process -Id $ProcessId -ErrorAction Stop
if ($game.ProcessName -ne 'Minecraft.Windows' -or
    -not $game.Path -or
    -not $game.Path.EndsWith($expectedImageSuffix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "PID $ProcessId is not the expected Minecraft.Windows.exe from package 1.26.5203.0 x64. No memory was read."
}

if (-not ('FovProbeNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public sealed class FovProbeCell
{
    public string Address { get; set; }
    public float Value { get; set; }
}

public sealed class FovProbeResult
{
    public long BytesRead { get; set; }
    public bool Truncated { get; set; }
    public List<FovProbeCell> Candidates { get; set; }
    public List<FovProbePattern> Patterns { get; set; }
}

public sealed class FovProbePattern
{
    public string Address { get; set; }
    public float[] Values { get; set; }
}

public sealed class FovProbeNeighbor
{
    public int Offset { get; set; }
    public float Value { get; set; }
}

public sealed class FovProbeAddressInfo
{
    public string Address { get; set; }
    public bool ReadSucceeded { get; set; }
    public uint BytesRead { get; set; }
    public string TargetBytesHex { get; set; }
    public float? TargetFloat { get; set; }
    public string AllocationBase { get; set; }
    public string RegionBase { get; set; }
    public ulong RegionSize { get; set; }
    public uint State { get; set; }
    public uint Protect { get; set; }
    public uint Type { get; set; }
    public List<FovProbeNeighbor> NearbyFloats { get; set; }
}

public static class FovProbeNative
{
    private const uint ProcessVmRead = 0x0010;
    private const uint ProcessQueryInformation = 0x0400;
    private const uint MemCommit = 0x1000;
    private const uint MemPrivate = 0x20000;
    private const uint MemImage = 0x1000000;
    private const uint PageReadWrite = 0x04;
    private const uint PageWriteCopy = 0x08;
    private const uint PageExecuteReadWrite = 0x40;
    private const uint PageExecuteWriteCopy = 0x80;
    private const ulong MaxBytes = 4UL * 1024UL * 1024UL * 1024UL;
    private const int ChunkBytes = 1024 * 1024;
    private const int MaxCandidates = 300000;

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryBasicInformation
    {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public ushort PartitionId;
        public UIntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern UIntPtr VirtualQueryEx(
        IntPtr process,
        IntPtr address,
        out MemoryBasicInformation information,
        UIntPtr informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadProcessMemory(
        IntPtr process,
        IntPtr baseAddress,
        [Out] byte[] buffer,
        UIntPtr size,
        out UIntPtr bytesRead);

    public static FovProbeResult Capture(int processId, float[] targetValues, float[] patternValues)
    {
        IntPtr process = OpenProcess(ProcessVmRead | ProcessQueryInformation, false, processId);
        if (process == IntPtr.Zero)
        {
            throw new InvalidOperationException("OpenProcess for reading failed: Win32 " + Marshal.GetLastWin32Error());
        }

        var result = new FovProbeResult
        {
            Candidates = new List<FovProbeCell>(),
            Patterns = new List<FovProbePattern>()
        };
        try
        {
            ulong cursor = 0x10000;
            const ulong maxAddress = 0x00007FFFFFFFFFFF;
            ulong informationSize = (ulong)Marshal.SizeOf<MemoryBasicInformation>();

            while (cursor < maxAddress && result.BytesRead < (long)MaxBytes && result.Candidates.Count < MaxCandidates)
            {
                var address = new IntPtr(unchecked((long)cursor));
                MemoryBasicInformation info;
                UIntPtr queryResult = VirtualQueryEx(process, address, out info, new UIntPtr(informationSize));
                if (queryResult == UIntPtr.Zero)
                {
                    break;
                }

                ulong regionBase = unchecked((ulong)info.BaseAddress.ToInt64());
                ulong regionSize = info.RegionSize.ToUInt64();
                ulong next = regionBase + regionSize;
                if (regionSize == 0 || next <= cursor)
                {
                    break;
                }

                uint protection = info.Protect & 0xFF;
                bool writable = protection == PageReadWrite || protection == PageWriteCopy ||
                    protection == PageExecuteReadWrite || protection == PageExecuteWriteCopy;
                bool localMemory = info.Type == MemPrivate || info.Type == MemImage;

                if (info.State == MemCommit && writable && localMemory)
                {
                    ulong offset = cursor > regionBase ? cursor - regionBase : 0;
                    while (offset < regionSize && result.BytesRead < (long)MaxBytes && result.Candidates.Count < MaxCandidates)
                    {
                        ulong remaining = Math.Min((ulong)ChunkBytes, regionSize - offset);
                        remaining = Math.Min(remaining, MaxBytes - (ulong)result.BytesRead);
                        if (remaining < 4)
                        {
                            break;
                        }

                        byte[] buffer = new byte[(int)remaining];
                        UIntPtr nativeRead;
                        bool readOk = ReadProcessMemory(
                            process,
                            new IntPtr(unchecked((long)(regionBase + offset))),
                            buffer,
                            new UIntPtr(remaining),
                            out nativeRead);
                        ulong count = nativeRead.ToUInt64();
                        if (count > 0)
                        {
                            result.BytesRead += (long)count;
                            int usable = (int)Math.Min(count, (ulong)buffer.Length);
                            for (int index = 0; index + 4 <= usable; index += 4)
                            {
                                float value = BitConverter.ToSingle(buffer, index);
                                bool candidate = false;
                                if (!float.IsNaN(value) && !float.IsInfinity(value))
                                {
                                    if (targetValues == null || targetValues.Length == 0)
                                    {
                                        candidate = value >= 10.0f && value <= 120.0f &&
                                            Math.Abs(value - Math.Round(value)) < 0.0001;
                                    }
                                    else
                                    {
                                        for (int targetIndex = 0; targetIndex < targetValues.Length; targetIndex++)
                                        {
                                            if (Math.Abs(value - targetValues[targetIndex]) < 0.0001)
                                            {
                                                candidate = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                if (candidate)
                                {
                                    result.Candidates.Add(new FovProbeCell
                                    {
                                        Address = "0x" + (regionBase + offset + (ulong)index).ToString("X"),
                                        Value = value
                                    });
                                }

                                if (patternValues != null && patternValues.Length > 1 &&
                                    index + patternValues.Length * 4 <= usable &&
                                    Math.Abs(value - patternValues[0]) < 0.0001)
                                {
                                    bool patternMatches = true;
                                    for (int patternIndex = 1; patternIndex < patternValues.Length; patternIndex++)
                                    {
                                        float patternValue = BitConverter.ToSingle(buffer, index + patternIndex * 4);
                                        if (float.IsNaN(patternValue) || float.IsInfinity(patternValue) ||
                                            Math.Abs(patternValue - patternValues[patternIndex]) >= 0.0001)
                                        {
                                            patternMatches = false;
                                            break;
                                        }
                                    }

                                    if (patternMatches)
                                    {
                                        result.Patterns.Add(new FovProbePattern
                                        {
                                            Address = "0x" + (regionBase + offset + (ulong)index).ToString("X"),
                                            Values = (float[])patternValues.Clone()
                                        });
                                    }
                                }
                            }
                        }

                        if (!readOk && count == 0)
                        {
                            offset += 4096;
                        }
                        else
                        {
                            offset += Math.Max(count, 4096UL);
                        }
                    }
                }

                cursor = next;
            }

            result.Truncated = result.BytesRead >= (long)MaxBytes || result.Candidates.Count >= MaxCandidates;
            return result;
        }
        finally
        {
            CloseHandle(process);
        }
    }

    public static List<FovProbeAddressInfo> Describe(int processId, ulong[] addresses)
    {
        IntPtr process = OpenProcess(ProcessVmRead | ProcessQueryInformation, false, processId);
        if (process == IntPtr.Zero)
        {
            throw new InvalidOperationException("OpenProcess for reading failed: Win32 " + Marshal.GetLastWin32Error());
        }

        var results = new List<FovProbeAddressInfo>();
        try
        {
            ulong informationSize = (ulong)Marshal.SizeOf<MemoryBasicInformation>();
            foreach (ulong target in addresses)
            {
                MemoryBasicInformation info;
                UIntPtr queryResult = VirtualQueryEx(
                    process,
                    new IntPtr(unchecked((long)target)),
                    out info,
                    new UIntPtr(informationSize));
                if (queryResult == UIntPtr.Zero)
                {
                    continue;
                }

                ulong regionBase = unchecked((ulong)info.BaseAddress.ToInt64());
                ulong regionSize = info.RegionSize.ToUInt64();
                ulong readStart = target >= 32 ? target - 32 : target;
                ulong readEnd = target + 36;
                if (readStart < regionBase) readStart = regionBase;
                if (readEnd > regionBase + regionSize) readEnd = regionBase + regionSize;

                int byteCount = (int)Math.Min(128UL, readEnd - readStart);
                byte[] buffer = new byte[byteCount];
                UIntPtr nativeRead;
                bool readSucceeded = ReadProcessMemory(process, new IntPtr(unchecked((long)readStart)), buffer, new UIntPtr((uint)byteCount), out nativeRead);
                int count = (int)Math.Min(nativeRead.ToUInt64(), (ulong)buffer.Length);
                int targetOffset = (int)(target - readStart);
                float? targetFloat = null;
                string targetBytesHex = "";
                if (targetOffset >= 0 && targetOffset + 4 <= count)
                {
                    targetFloat = BitConverter.ToSingle(buffer, targetOffset);
                    targetBytesHex = BitConverter.ToString(buffer, targetOffset, 4).Replace("-", "");
                }
                int firstAligned = (int)((4 - (readStart & 3)) & 3);
                var neighbors = new List<FovProbeNeighbor>();
                for (int offset = firstAligned; offset + 4 <= count; offset += 4)
                {
                    float value = BitConverter.ToSingle(buffer, offset);
                    if (!float.IsNaN(value) && !float.IsInfinity(value) && Math.Abs(value) <= 1000000.0f)
                    {
                        neighbors.Add(new FovProbeNeighbor
                        {
                            Offset = (int)(readStart + (ulong)offset - target),
                            Value = value
                        });
                    }
                }

                results.Add(new FovProbeAddressInfo
                {
                    Address = "0x" + target.ToString("X"),
                    ReadSucceeded = readSucceeded,
                    BytesRead = (uint)count,
                    TargetBytesHex = targetBytesHex,
                    TargetFloat = targetFloat,
                    AllocationBase = "0x" + unchecked((ulong)info.AllocationBase.ToInt64()).ToString("X"),
                    RegionBase = "0x" + regionBase.ToString("X"),
                    RegionSize = regionSize,
                    State = info.State,
                    Protect = info.Protect,
                    Type = info.Type,
                    NearbyFloats = neighbors
                });
            }

            return results;
        }
        finally
        {
            CloseHandle(process);
        }
    }
}
'@
}

if ($Mode -eq 'Describe') {
    if (-not $AddressesCsv) { throw 'Mode Describe requires -AddressesCsv.' }
    $addresses = @($AddressesCsv.Split(',') | ForEach-Object {
        $hex = $_.Trim().Replace('0x', '').Replace('0X', '')
        [Convert]::ToUInt64($hex, 16)
    })
    [FovProbeNative]::Describe($ProcessId, [UInt64[]]$addresses) |
        ConvertTo-Json -Depth 5
}
elseif ($Mode -eq 'Capture') {
    $snapshot = [FovProbeNative]::Capture($ProcessId, $CandidateValues, $PatternValues)
    $document = [pscustomobject]@{
        ProcessId = $ProcessId
        ImagePath = $game.Path
        CapturedAtUtc = [DateTime]::UtcNow.ToString('o')
        TargetValues = @($CandidateValues)
        PatternValues = @($PatternValues)
        BytesRead = $snapshot.BytesRead
        Truncated = $snapshot.Truncated
        CandidateCount = $snapshot.Candidates.Count
        Candidates = $snapshot.Candidates
        PatternCount = $snapshot.Patterns.Count
        Patterns = $snapshot.Patterns
    }
    $resolvedPath = [IO.Path]::GetFullPath($BeforePath)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedPath) | Out-Null
    $document | ConvertTo-Json -Depth 5 -Compress | Set-Content -LiteralPath $resolvedPath -Encoding utf8
    Write-Host "Read-only snapshot saved: $resolvedPath"
    Write-Host "Bytes read: $($document.BytesRead); targeted FOV candidates: $($document.CandidateCount); structure matches: $($document.PatternCount); capped: $($document.Truncated)"
}
else {
    $before = Get-Content -LiteralPath ([IO.Path]::GetFullPath($BeforePath)) -Raw | ConvertFrom-Json
    $after = Get-Content -LiteralPath ([IO.Path]::GetFullPath($AfterPath)) -Raw | ConvertFrom-Json
    if ($before.ProcessId -ne $after.ProcessId -or $before.ProcessId -ne $ProcessId) {
        throw 'The snapshots do not belong to the same verified PID.'
    }

    $oldValues = @{}
    foreach ($cell in $before.Candidates) { $oldValues[$cell.Address] = [float]$cell.Value }
    $changes = foreach ($cell in $after.Candidates) {
        if ($oldValues.ContainsKey($cell.Address)) {
            $old = [float]$oldValues[$cell.Address]
            $new = [float]$cell.Value
            if ([math]::Abs($new - $old) -ge 1.0) {
                [pscustomobject]@{ Address = $cell.Address; Before = $old; After = $new; Delta = ($new - $old) }
            }
        }
    }

    $reportedChanges = @($changes)
    $hasExpectedPair = -not [float]::IsNaN($ExpectedBefore) -and -not [float]::IsInfinity($ExpectedBefore) -and
        -not [float]::IsNaN($ExpectedAfter) -and -not [float]::IsInfinity($ExpectedAfter)
    if ($hasExpectedPair) {
        $reportedChanges = @($changes | Where-Object {
            [math]::Abs($_.Before - $ExpectedBefore) -lt 0.001 -and
            [math]::Abs($_.After - $ExpectedAfter) -lt 0.001
        })
        $beforeMatches = @($before.Candidates | Where-Object { [math]::Abs([float]$_.Value - $ExpectedBefore) -lt 0.001 })
        $afterMatches = @($after.Candidates | Where-Object { [math]::Abs([float]$_.Value - $ExpectedAfter) -lt 0.001 })
        $oldToOther = @($changes | Where-Object { [math]::Abs($_.Before - $ExpectedBefore) -lt 0.001 })
        $otherToNew = @($changes | Where-Object { [math]::Abs($_.After - $ExpectedAfter) -lt 0.001 })
        Write-Host "Slots with old value: $($beforeMatches.Count); slots with new value: $($afterMatches.Count); old changed elsewhere: $($oldToOther.Count); new changed from elsewhere: $($otherToNew.Count)"
        if ($otherToNew.Count -gt 0) {
            $otherToNew | Select-Object Address,Before,After | Format-Table -AutoSize
        }
        Write-Host "Direct-degree matches for $ExpectedBefore -> ${ExpectedAfter}: $($reportedChanges.Count)"
    }

    $reportedChanges | Group-Object { '{0:R} -> {1:R}' -f $_.Before, $_.After } |
        Sort-Object Count -Descending | Select-Object -First 20 Name,Count | Format-Table -AutoSize
    Write-Host "Changed float candidates: $(@($changes).Count). This comparison does not write to the game process."
}
