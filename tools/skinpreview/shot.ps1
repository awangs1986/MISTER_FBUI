# 用 Edge 无头模式给预览页截图，方便在没有图形界面时检查渲染结果。
#   .\shot.ps1 out.png [宽 高] [注入的 JS]
param(
	[string]$Out = "shot.png",
	[int]$Width = 1600,
	[int]$Height = 1200,
	[string]$Js = ""
)

$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$page = Join-Path $here "preview.html"
$target = Join-Path $here $Out

# 需要先跑一段脚本时，把它塞进一份临时副本里（无头模式没法交互点击）
if ($Js -ne "") {
	$tmp = Join-Path $here ".shot.html"
	$html = Get-Content $page -Raw -Encoding UTF8
	$html = $html -replace '(?s)</body>', "<script>$Js</script></body>"
	[System.IO.File]::WriteAllText($tmp, $html, [System.Text.UTF8Encoding]::new($false))
	$page = $tmp
}

& $edge --headless=new --disable-gpu --hide-scrollbars `
	--window-size="$Width,$Height" --screenshot="$target" `
	--virtual-time-budget=4000 --allow-file-access-from-files "file:///$($page -replace '\\','/')" 2>&1 |
	Where-Object { $_ -notmatch 'DevTools|GPU|Fontconfig|voice|Registration' }

if ($Js -ne "") { Remove-Item $tmp -ErrorAction SilentlyContinue }
if (Test-Path $target) { "OK $target $((Get-Item $target).Length) bytes" } else { "FAILED" }
