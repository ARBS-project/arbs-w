# SPDX-License-Identifier: GPL-3.0-or-later
# ARBS-W R1b (D-R1b-1): applies patches\moonlight-common-c-6f05ee89-arbs.patch to the moonlight-common-c submodule without
# moving its public commit. Idempotent. Stops unless the patched tree is exactly the expected tree, in the index and on disk.
$ErrorActionPreference = 'Continue'

$BaseCommit = '6f05ee8947d1b2d7b69fd4983029d0447e521997'
$ExpectedTree = 'a7fc9110d1418bf75e569a9f04aeebe5fc08ca70'

$repo = Split-Path -Parent $PSScriptRoot
$sub = Join-Path $repo 'moonlight-common-c\moonlight-common-c'
$patch = Join-Path $repo 'patches\moonlight-common-c-6f05ee89-arbs.patch'
$tmpIndex = Join-Path ([IO.Path]::GetTempPath()) ('arbs-w-index-' + [guid]::NewGuid().ToString('N'))

function Fail([string]$Message, [int]$Code) {
    Write-Output ('[arbs-w-patches] ERROR: ' + $Message)
    Remove-Item Env:\GIT_INDEX_FILE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tmpIndex -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath ($tmpIndex + '.lock') -Force -ErrorAction SilentlyContinue
    exit $Code
}

function Test-WorkTreeMatches {
    & git -C $sub update-index -q --refresh 2>$null | Out-Null
    & git -C $sub diff-files --quiet --ignore-submodules=all 2>$null
    return ($LASTEXITCODE -eq 0)
}

if (-not (Test-Path -LiteralPath $patch -PathType Leaf)) { Fail 'patch file not found' 2 }
$head = (& git -C $sub rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0) { Fail 'moonlight-common-c submodule is not initialized' 3 }
if ($head.Trim() -ne $BaseCommit) { Fail ('submodule is not at the expected public commit ' + $BaseCommit) 4 }

$env:GIT_INDEX_FILE = $tmpIndex
& git -C $sub read-tree $BaseCommit
if ($LASTEXITCODE -ne 0) { Fail 'read-tree failed' 5 }
& git -C $sub apply --cached --whitespace=nowarn $patch
if ($LASTEXITCODE -ne 0) { Fail 'patch does not apply to the base commit' 6 }
$tree = (& git -C $sub write-tree)
if ($LASTEXITCODE -ne 0) { Fail 'write-tree failed' 7 }
if ($tree.Trim() -ne $ExpectedTree) { Fail ('patched tree ' + $tree.Trim() + ' differs from ' + $ExpectedTree) 8 }

if (Test-WorkTreeMatches) {
    Write-Output ('[arbs-w-patches] already applied, tree ' + $ExpectedTree)
} else {
    Remove-Item Env:\GIT_INDEX_FILE
    & git -C $sub apply --whitespace=nowarn $patch
    if ($LASTEXITCODE -ne 0) { Fail 'patch does not apply to the work tree (local changes?)' 9 }
    $env:GIT_INDEX_FILE = $tmpIndex
    if (-not (Test-WorkTreeMatches)) { Fail 'work tree differs from the expected tree after patching' 10 }
    Write-Output ('[arbs-w-patches] applied, tree ' + $ExpectedTree)
}

Remove-Item Env:\GIT_INDEX_FILE -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $tmpIndex -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath ($tmpIndex + '.lock') -Force -ErrorAction SilentlyContinue
exit 0
