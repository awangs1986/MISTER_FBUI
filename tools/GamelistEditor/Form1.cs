using System.Globalization;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Xml.Linq;

namespace GamelistEditor;

public partial class Form1 : Form
{
    private readonly TextBox _txtIp = new();
    private readonly TextBox _txtUser = new();
    private readonly TextBox _txtPass = new();
    private readonly Button _btnConnect = new();
    private readonly ComboBox _cboSystem = new();
    private readonly ComboBox _cboThumbType = new();
    private readonly ComboBox _cboImageConflict = new();
    private readonly Button _btnReload = new();
    private readonly Button _btnSave = new();
    private readonly Button _btnFetchThumbs = new();
    private readonly CheckBox _chkOnlyRom = new();
    private readonly DataGridView _grid = new();
    private readonly PictureBox _preview = new();
    private readonly Label _lblStatus = new();
    private readonly ProgressBar _progress = new();

    private readonly List<GameRow> _rows = new();
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(30) };
    private CancellationTokenSource? _fetchCts;
    private bool _dirty;
    private bool _fetching;
    private string _currentSystem = "";
    private string _tempPreview = "";

    private const string GamesRoot = "/media/fat/games";

    public Form1()
    {
        InitializeComponent();
        Text = "MiSTer Gamelist 编辑器 — 截图版 2026.07.24b（括号外匹配）";
        MinimumSize = new Size(900, 560);
        Width = 1180;
        Height = 760;
        StartPosition = FormStartPosition.CenterScreen;
        // Inherit one font; Autoscale Font keeps control sizes in sync with glyphs.
        Font = new Font("Microsoft YaHei UI", 9f, FontStyle.Regular, GraphicsUnit.Point);
        AutoScaleMode = AutoScaleMode.Font;
        AutoScaleDimensions = new SizeF(7F, 15F);
        _http.DefaultRequestHeaders.UserAgent.ParseAdd("MiSTerGamelistEditor/1.0");

        var top = BuildToolbar();

        _preview.Dock = DockStyle.Right;
        _preview.Width = 260;
        _preview.SizeMode = PictureBoxSizeMode.Zoom;
        _preview.BackColor = Color.FromArgb(30, 30, 30);
        _preview.BorderStyle = BorderStyle.FixedSingle;

        _grid.Dock = DockStyle.Fill;
        _grid.AllowUserToAddRows = false;
        _grid.AllowUserToDeleteRows = false;
        _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill;
        _grid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
        _grid.MultiSelect = false;
        _grid.RowHeadersVisible = false;
        _grid.BackgroundColor = Color.White;
        _grid.RowTemplate.Height = 26;
        _grid.ColumnHeadersHeight = 28;
        _grid.SelectionChanged += async (_, _) => await ShowPreviewAsync();
        _grid.CellValueChanged += (_, e) =>
        {
            if (e.RowIndex < 0) return;
            _dirty = true;
            UpdateStatus();
        };
        _grid.CurrentCellDirtyStateChanged += (_, _) =>
        {
            if (_grid.IsCurrentCellDirty) _grid.CommitEdit(DataGridViewDataErrorContexts.Commit);
        };

        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 52, Padding = new Padding(8, 4, 8, 4) };
        _progress.Dock = DockStyle.Top;
        _progress.Height = 14;
        _progress.Visible = false;
        _lblStatus.Dock = DockStyle.Fill;
        _lblStatus.TextAlign = ContentAlignment.MiddleLeft;
        _lblStatus.AutoEllipsis = true;
        bottom.Controls.Add(_lblStatus);
        bottom.Controls.Add(_progress);

        Controls.Add(_grid);
        Controls.Add(_preview);
        Controls.Add(bottom);
        Controls.Add(top);

        BuildColumns();
        UpdateStatus();
        FormClosing += (_, e) =>
        {
            if (_fetching)
            {
                _fetchCts?.Cancel();
            }
            if (!_dirty) return;
            var r = MessageBox.Show(this, "有未保存修改，确定退出？", "确认",
                MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (r != DialogResult.Yes) e.Cancel = true;
        };
    }

    private static Label MakeLbl(string text)
    {
        return new Label
        {
            Text = text,
            AutoSize = true,
            Margin = new Padding(6, 8, 2, 0),
            TextAlign = ContentAlignment.MiddleLeft
        };
    }

    private Panel BuildToolbar()
    {
        // Flow layouts avoid fixed-height buttons that clip under HiDPI font scaling.
        var top = new Panel { Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(6) };

        FlowLayoutPanel Row() => new()
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            WrapContents = false,
            FlowDirection = FlowDirection.LeftToRight,
            Padding = new Padding(0),
            Margin = new Padding(0, 0, 0, 4)
        };

        var row1 = Row();
        row1.Controls.Add(MakeLbl("IP"));
        _txtIp.Text = "192.168.5.4";
        _txtIp.Width = 120;
        _txtIp.Margin = new Padding(2, 4, 8, 0);
        row1.Controls.Add(_txtIp);
        row1.Controls.Add(MakeLbl("用户"));
        _txtUser.Text = "root";
        _txtUser.Width = 64;
        _txtUser.Margin = new Padding(2, 4, 8, 0);
        row1.Controls.Add(_txtUser);
        row1.Controls.Add(MakeLbl("密码"));
        _txtPass.Text = "1";
        _txtPass.UseSystemPasswordChar = true;
        _txtPass.Width = 64;
        _txtPass.Margin = new Padding(2, 4, 8, 0);
        row1.Controls.Add(_txtPass);
        ConfigureAsAutoButton(_btnConnect, "连接", 64);
        row1.Controls.Add(_btnConnect);
        _btnConnect.Click += async (_, _) => await ConnectAsync();

        var row2 = Row();
        row2.Controls.Add(MakeLbl("主机"));
        _cboSystem.DropDownStyle = ComboBoxStyle.DropDownList;
        _cboSystem.Width = 160;
        _cboSystem.Margin = new Padding(2, 4, 8, 0);
        row2.Controls.Add(_cboSystem);
        row2.Controls.Add(MakeLbl("图类型"));
        _cboThumbType.DropDownStyle = ComboBoxStyle.DropDownList;
        _cboThumbType.Items.Clear();
        _cboThumbType.Items.AddRange(new object[] { "Named_Boxarts", "Named_Titles", "Named_Snaps" });
        // Screenshots suit FBUI's full-bleed game browser better than box art.
        _cboThumbType.SelectedIndex = 2;
        _cboThumbType.Width = 140;
        _cboThumbType.Margin = new Padding(2, 4, 8, 0);
        row2.Controls.Add(_cboThumbType);
        row2.Controls.Add(MakeLbl("冲突"));
        _cboImageConflict.DropDownStyle = ComboBoxStyle.DropDownList;
        _cboImageConflict.Items.AddRange(new object[]
        {
            "覆盖已有图片（全部替换）",
            "跳过已有图片（只补缺）"
        });
        _cboImageConflict.SelectedIndex = 0;
        _cboImageConflict.Width = 190;
        _cboImageConflict.Margin = new Padding(2, 4, 8, 0);
        row2.Controls.Add(_cboImageConflict);

        ConfigureAsAutoButton(_btnReload, "刷新", 60);
        ConfigureAsAutoButton(_btnSave, "保存覆盖", 88);
        ConfigureAsAutoButton(_btnFetchThumbs, "下载缩略图到 MiSTer", 160);
        row2.Controls.Add(_btnReload);
        row2.Controls.Add(_btnSave);
        row2.Controls.Add(_btnFetchThumbs);

        _btnReload.Click += async (_, _) =>
        {
            if (_cboSystem.SelectedItem is string sys)
                await LoadSystemAsync(sys, force: true);
        };
        _btnSave.Click += async (_, _) => await SaveAsync();
        _btnFetchThumbs.Click += async (_, _) => await FetchThumbnailsAsync();
        _cboSystem.SelectedIndexChanged += async (_, _) =>
        {
            if (_cboSystem.SelectedItem is string sys && sys != _currentSystem)
                await LoadSystemAsync(sys);
        };

        var row3 = Row();
        _chkOnlyRom.Text = "只显示卡上有的 ROM";
        _chkOnlyRom.AutoSize = true;
        _chkOnlyRom.Margin = new Padding(4, 6, 12, 0);
        _chkOnlyRom.CheckedChanged += (_, _) => RefreshGrid();
        row3.Controls.Add(_chkOnlyRom);
        var hint = new Label
        {
            Text = "建议图类型选 Named_Snaps；覆盖模式可将旧封面批量替换为游戏截图",
            AutoSize = true,
            ForeColor = Color.DimGray,
            Margin = new Padding(4, 8, 0, 0)
        };
        row3.Controls.Add(hint);

        // Dock order: last added is closest to top when Dock=Top → add bottom row first
        top.Controls.Add(row3);
        top.Controls.Add(row2);
        top.Controls.Add(row1);
        return top;
    }

    private static void ConfigureAsAutoButton(Button b, string text, int minWidth)
    {
        b.Text = text;
        b.AutoSize = true;
        b.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        b.FlatStyle = FlatStyle.Standard;
        b.UseVisualStyleBackColor = true;
        b.Margin = new Padding(3, 2, 3, 2);
        b.Padding = new Padding(12, 4, 12, 4);
        b.MinimumSize = new Size(minWidth, 0);
        // Critical: do NOT set Height — let AutoSize fit the scaled font.
        b.ResetFont(); // inherit Form font so DPI/font autoscaling applies once
    }

    private void BuildColumns()
    {
        _grid.Columns.Clear();
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            Name = "File", HeaderText = "ROM 文件名 (英文)", ReadOnly = true, FillWeight = 44
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            Name = "Display", HeaderText = "显示名 (可中文)", FillWeight = 36
        });
        _grid.Columns.Add(new DataGridViewCheckBoxColumn
        {
            Name = "HasRom", HeaderText = "ROM", ReadOnly = true, FillWeight = 7
        });
        _grid.Columns.Add(new DataGridViewCheckBoxColumn
        {
            Name = "HasImage", HeaderText = "图", ReadOnly = true, FillWeight = 6
        });
        _grid.Columns.Add(new DataGridViewCheckBoxColumn
        {
            Name = "HasVideo", HeaderText = "视频", ReadOnly = true, FillWeight = 7
        });
    }

    private NetworkCredential Creds() =>
        new(_txtUser.Text.Trim(), _txtPass.Text);

    private string Host()
    {
        var ip = _txtIp.Text.Trim();
        if (ip.StartsWith("ftp://", StringComparison.OrdinalIgnoreCase))
            ip = ip[6..];
        return ip.TrimEnd('/');
    }

    private string SystemDir(string system) => $"{GamesRoot}/{system}";
    private string GamelistPath(string system) => $"{SystemDir(system)}/gamelist.xml";

    private static string? LibretroPlaylist(string misterSystem) =>
        ThumbMatch.TryGetSys(misterSystem, out var info) ? info.Playlist : null;

    private async Task ConnectAsync()
    {
        try
        {
            UseWaitCursor = true;
            _lblStatus.Text = $"正在连接 {Host()} …";
            var dirs = await FtpListNamesAsync($"{GamesRoot}/");
            var systems = dirs
                .Where(d => !string.IsNullOrWhiteSpace(d) && !d.StartsWith('.'))
                .Select(NormalizeName)
                .Where(d => d.Length > 0 && d is not ("." or ".."))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(d => d, StringComparer.OrdinalIgnoreCase)
                .ToList();

            if (systems.Count == 0)
                throw new InvalidOperationException($"FTP 下列不出 {GamesRoot}/，请检查 IP/账号或 FTP 是否开启。");

            _cboSystem.Items.Clear();
            foreach (var s in systems) _cboSystem.Items.Add(s);

            var prefer = systems.FirstOrDefault(s => s.Equals("NES", StringComparison.OrdinalIgnoreCase))
                         ?? systems[0];
            _cboSystem.SelectedItem = prefer;
            _lblStatus.Text = $"已连接 {Host()}，共 {systems.Count} 个系统。下拉选择主机。";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "连接失败:\n" + ex.Message, "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
            UpdateStatus();
        }
        finally
        {
            UseWaitCursor = false;
        }
    }

    private async Task LoadSystemAsync(string system, bool force = false)
    {
        if (string.IsNullOrWhiteSpace(system)) return;
        if (_fetching)
        {
            MessageBox.Show(this, "正在下载缩略图，请稍候或先取消。", "提示",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            _cboSystem.SelectedItem = _currentSystem;
            return;
        }
        if (!force && _dirty)
        {
            var r = MessageBox.Show(this, $"切换到 {system} 将丢弃未保存修改，继续？", "确认",
                MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (r != DialogResult.Yes)
            {
                _cboSystem.SelectedItem = _currentSystem;
                return;
            }
        }

        try
        {
            UseWaitCursor = true;
            _currentSystem = system;
            _rows.Clear();
            _grid.Rows.Clear();
            ClearPreview();
            _lblStatus.Text = $"正在加载 {system}/gamelist.xml …";

            string? xmlText = null;
            try
            {
                xmlText = await FtpDownloadStringAsync(GamelistPath(system));
            }
            catch (WebException)
            {
                // no gamelist yet
            }

            var roms = await FtpListFilesAsync(SystemDir(system) + "/");
            var images = await FtpListFilesAsync($"{SystemDir(system)}/images/");
            var videos = await FtpListFilesAsync($"{SystemDir(system)}/videos/");
            var imageBases = ToBaseNameSet(images, new[] { ".png", ".jpg", ".jpeg", ".webp" });
            var videoBases = ToBaseNameSet(videos, new[] { ".mp4", ".mkv", ".avi", ".webm" });
            var romSet = roms
                .Where(f => !f.Equals("gamelist.xml", StringComparison.OrdinalIgnoreCase))
                .Where(f => !f.EndsWith(".bak", StringComparison.OrdinalIgnoreCase))
                .Where(f => !f.Equals("images", StringComparison.OrdinalIgnoreCase))
                .Where(f => !f.Equals("videos", StringComparison.OrdinalIgnoreCase))
                .Where(f => !f.Equals("Palettes", StringComparison.OrdinalIgnoreCase))
                .ToHashSet(StringComparer.OrdinalIgnoreCase);

            if (!string.IsNullOrEmpty(xmlText))
            {
                var doc = XDocument.Parse(xmlText);
                foreach (var g in doc.Root?.Elements("game") ?? Enumerable.Empty<XElement>())
                {
                    var path = (string?)g.Element("path") ?? "";
                    var name = (string?)g.Element("name") ?? "";
                    var file = Path.GetFileName(path.Replace('\\', '/').Trim());
                    if (file.StartsWith("./")) file = file[2..];
                    if (string.IsNullOrEmpty(file)) continue;
                    if (string.IsNullOrWhiteSpace(name)) name = Path.GetFileNameWithoutExtension(file);
                    var baseName = Path.GetFileNameWithoutExtension(file);
                    _rows.Add(new GameRow
                    {
                        FileName = file,
                        DisplayName = name,
                        HasRom = romSet.Contains(file),
                        HasImage = imageBases.Contains(baseName),
                        HasVideo = videoBases.Contains(baseName)
                    });
                }
            }

            var known = _rows.Select(r => r.FileName).ToHashSet(StringComparer.OrdinalIgnoreCase);
            foreach (var rom in romSet.OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
            {
                if (known.Contains(rom)) continue;
                var baseName = Path.GetFileNameWithoutExtension(rom);
                _rows.Add(new GameRow
                {
                    FileName = rom,
                    DisplayName = baseName,
                    HasRom = true,
                    HasImage = imageBases.Contains(baseName),
                    HasVideo = videoBases.Contains(baseName)
                });
            }

            _rows.Sort((a, b) => string.Compare(a.FileName, b.FileName, StringComparison.OrdinalIgnoreCase));
            _dirty = false;
            RefreshGrid();
            UpdateStatus();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"加载 {system} 失败:\n" + ex.Message, "错误",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            UpdateStatus();
        }
        finally
        {
            UseWaitCursor = false;
        }
    }

    private async Task SaveAsync()
    {
        if (string.IsNullOrEmpty(_currentSystem))
        {
            MessageBox.Show(this, "请先连接并选择主机系统。", "提示", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        PullGridEdits();
        try
        {
            UseWaitCursor = true;
            var remote = GamelistPath(_currentSystem);
            try
            {
                var existing = await FtpDownloadStringAsync(remote);
                await FtpUploadStringAsync(remote + ".bak", existing);
            }
            catch { /* no existing */ }

            var sb = new StringBuilder();
            sb.AppendLine("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
            sb.AppendLine("<!-- Edited by GamelistEditor: path=English ROM, name=display -->");
            sb.AppendLine("<gameList>");
            foreach (var r in _rows.OrderBy(x => x.FileName, StringComparer.OrdinalIgnoreCase))
            {
                var baseName = Path.GetFileNameWithoutExtension(r.FileName);
                var disp = string.IsNullOrWhiteSpace(r.DisplayName) ? baseName : r.DisplayName.Trim();
                sb.AppendLine("  <game>");
                sb.AppendLine($"    <path>./{XmlEscape(r.FileName)}</path>");
                sb.AppendLine($"    <name>{XmlEscape(disp)}</name>");
                sb.AppendLine($"    <image>./images/{XmlEscape(baseName)}.png</image>");
                sb.AppendLine($"    <video>./videos/{XmlEscape(baseName)}.mp4</video>");
                sb.AppendLine("  </game>");
            }
            sb.AppendLine("</gameList>");

            await FtpUploadStringAsync(remote, sb.ToString());
            _dirty = false;
            UpdateStatus();
            MessageBox.Show(this,
                $"已覆盖保存到 MiSTer:\n{remote}\n(原文件备份为 gamelist.xml.bak)",
                "保存成功", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "保存失败:\n" + ex.Message, "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            UseWaitCursor = false;
        }
    }

    private async Task FetchThumbnailsAsync()
    {
        if (_fetching)
        {
            _fetchCts?.Cancel();
            return;
        }
        if (string.IsNullOrEmpty(_currentSystem))
        {
            MessageBox.Show(this, "请先连接并选择主机。", "提示", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        var playlist = LibretroPlaylist(_currentSystem);
        if (playlist == null)
        {
            MessageBox.Show(this,
                $"主机「{_currentSystem}」尚未映射到 Libretro Thumbnails。\n可在 ThumbMatch.Systems 中补充。",
                "不支持", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        var thumbType = _cboThumbType.SelectedItem as string ?? "Named_Snaps";
        var overwriteExisting = _cboImageConflict.SelectedIndex == 0;
        var candidates = _rows.Where(r => r.HasRom).ToList();
        if (!overwriteExisting)
            candidates = candidates.Where(r => !r.HasImage).ToList();

        if (candidates.Count == 0)
        {
            MessageBox.Show(this, "没有需要下载的条目（跳过模式下已有图片不会重复下载）。",
                "提示", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        var datRoot = ThumbMatch.FindDataRoot();
        var hasDat = ThumbMatch.TryGetSys(_currentSystem, out var sysInfo) && sysInfo.DatRelativePath != null && datRoot != null;
        var outsideOnly = ThumbMatch.UsesOutsideParenthesesOnly(_currentSystem);
        var confirm = MessageBox.Show(this,
            $"将为 {_currentSystem} 的 {candidates.Count} 个 ROM\n从 Libretro「{thumbType}」下载并上传到 MiSTer。\n\n" +
            $"文件冲突：{(overwriteExisting ? "覆盖已有图片" : "跳过已有图片")}\n" +
            (outsideOnly
                ? "匹配规则：删除所有括号及括号内容，只按括号外游戏名匹配\n"
                : $"匹配顺序：本地覆盖表 → {(hasDat ? "ROM CRC 查 DAT" : "无 DAT（跳过哈希）")} → 去后缀文件名 → setname\n") +
            $"数据目录：{(datRoot ?? "未找到 tools/data")}\n继续？",
            "下载缩略图", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
        if (confirm != DialogResult.Yes) return;

        _fetching = true;
        _fetchCts = new CancellationTokenSource();
        var ct = _fetchCts.Token;
        _btnFetchThumbs.Text = "停止下载";
        _progress.Visible = true;
        _progress.Minimum = 0;
        _progress.Maximum = candidates.Count;
        _progress.Value = 0;

        var ok = 0;
        var miss = 0;
        var fail = 0;
        var done = 0;
        string? lastFail = null;
        string? lastMissUrl = null;
        using var ftpGate = new SemaphoreSlim(1, 1);

        try
        {
            await FtpEnsureDirAsync($"{SystemDir(_currentSystem)}/images");

            async Task OneAsync(GameRow row)
            {
                ct.ThrowIfCancellationRequested();
                var baseName = Path.GetFileNameWithoutExtension(row.FileName);
                var remoteRom = $"{SystemDir(_currentSystem)}/{row.FileName}";
                var remoteImg = $"{SystemDir(_currentSystem)}/images/{baseName}.png";
                try
                {
                    byte[]? romBytes = null;
                    if (hasDat)
                    {
                        await ftpGate.WaitAsync(ct);
                        try
                        {
                            var sz = await FtpSizeAsync(remoteRom);
                            if (sz is > 0 && ThumbMatch.ShouldHash(sz.Value))
                                romBytes = await FtpDownloadBytesAsync(remoteRom);
                        }
                        catch
                        {
                            // hash optional — fall back to filename rules
                        }
                        finally
                        {
                            ftpGate.Release();
                        }
                    }

                    var names = ThumbMatch.ResolveNames(_currentSystem, row.FileName, romBytes);

                    byte[]? bytes = null;
                    foreach (var tryName in names)
                    {
                        var url = ThumbMatch.BuildThumbUrl(playlist, thumbType, tryName);
                        lastMissUrl = url;
                        using var resp = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
                        if (resp.StatusCode == HttpStatusCode.NotFound)
                            continue;
                        resp.EnsureSuccessStatusCode();
                        var data = await resp.Content.ReadAsByteArrayAsync(ct);
                        if (data.Length < 32 || data[0] != 0x89)
                            continue;
                        bytes = data;
                        break;
                    }

                    if (bytes == null)
                    {
                        Interlocked.Increment(ref miss);
                        return;
                    }

                    await ftpGate.WaitAsync(ct);
                    try
                    {
                        await FtpUploadBytesAsync(remoteImg, bytes);
                    }
                    finally
                    {
                        ftpGate.Release();
                    }

                    row.HasImage = true;
                    Interlocked.Increment(ref ok);
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex)
                {
                    Interlocked.Increment(ref fail);
                    lastFail = $"{row.FileName}: {ex.Message}";
                }
                finally
                {
                    var n = Interlocked.Increment(ref done);
                    BeginInvoke(() =>
                    {
                        _progress.Value = Math.Min(n, _progress.Maximum);
                        _lblStatus.Text =
                            $"下载中 {n}/{candidates.Count}  成功 {ok}  无图 {miss}  失败 {fail}  |  {row.FileName}";
                    });
                }
            }

            using var httpGate = new SemaphoreSlim(4, 4);
            var tasks = candidates.Select(async row =>
            {
                await httpGate.WaitAsync(ct);
                try { await OneAsync(row); }
                finally { httpGate.Release(); }
            });
            await Task.WhenAll(tasks);

            RefreshGrid();
            var msg = $"完成。成功 {ok}，库中无图 {miss}，上传/网络失败 {fail}。\n" +
                      $"已写入 /media/fat/games/{_currentSystem}/images/";
            if (ok == 0 && miss > 0)
                msg += "\n\n提示：无图 = Libretro 没有对应缩略图（不是上传失败）。\n" +
                       "可在 tools/data/thumb_overrides.json 为个别 ROM 补映射。" +
                       (lastMissUrl != null ? "\n最后尝试地址：\n" + lastMissUrl : "");
            if (fail > 0 && lastFail != null)
                msg += "\n\n最后一次失败原因:\n" + lastFail;
            MessageBox.Show(this, msg, "下载结束", MessageBoxButtons.OK,
                fail > 0 || ok == 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
        }
        catch (OperationCanceledException)
        {
            RefreshGrid();
            MessageBox.Show(this, $"已停止。成功 {ok}，无图 {miss}，失败 {fail}。", "已取消",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "下载过程出错:\n" + ex.Message, "错误",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            _fetching = false;
            _fetchCts?.Dispose();
            _fetchCts = null;
            _btnFetchThumbs.Text = "下载缩略图到 MiSTer";
            _progress.Visible = false;
            UpdateStatus();
        }
    }

    private void RefreshGrid()
    {
        PullGridEdits();
        _grid.Rows.Clear();
        IEnumerable<GameRow> view = _rows;
        if (_chkOnlyRom.Checked) view = view.Where(r => r.HasRom);
        foreach (var r in view)
        {
            var i = _grid.Rows.Add(r.FileName, r.DisplayName, r.HasRom, r.HasImage, r.HasVideo);
            _grid.Rows[i].Tag = r;
        }
        UpdateStatus();
    }

    private void PullGridEdits()
    {
        foreach (DataGridViewRow row in _grid.Rows)
        {
            if (row.Tag is not GameRow gr) continue;
            var disp = Convert.ToString(row.Cells["Display"].Value, CultureInfo.InvariantCulture) ?? "";
            gr.DisplayName = disp.Trim();
        }
    }

    private async Task ShowPreviewAsync()
    {
        ClearPreview();
        if (_grid.CurrentRow?.Tag is not GameRow gr || string.IsNullOrEmpty(_currentSystem)) return;
        if (!gr.HasImage) return;

        var baseName = Path.GetFileNameWithoutExtension(gr.FileName);
        foreach (var ext in new[] { ".png", ".jpg", ".jpeg", ".webp" })
        {
            var remote = $"{SystemDir(_currentSystem)}/images/{baseName}{ext}";
            try
            {
                var local = Path.Combine(Path.GetTempPath(), "fbui_preview" + ext);
                await FtpDownloadFileAsync(remote, local);
                using (var fs = File.OpenRead(local))
                    _preview.Image = new Bitmap(fs);
                _tempPreview = local;
                return;
            }
            catch
            {
                // try next
            }
        }
    }

    private void ClearPreview()
    {
        if (_preview.Image != null)
        {
            var old = _preview.Image;
            _preview.Image = null;
            old.Dispose();
        }
        if (!string.IsNullOrEmpty(_tempPreview) && File.Exists(_tempPreview))
        {
            try { File.Delete(_tempPreview); } catch { /* ignore */ }
            _tempPreview = "";
        }
    }

    private void UpdateStatus()
    {
        if (string.IsNullOrEmpty(_currentSystem))
        {
            _lblStatus.Text = "填 IP → 连接 → 选主机 → 改显示名 / 下载缩略图 → 保存覆盖。";
            return;
        }
        var mapped = LibretroPlaylist(_currentSystem);
        var local = _rows.Count(r => r.HasRom);
        var shown = _chkOnlyRom.Checked ? local : _rows.Count;
        _lblStatus.Text =
            $"{Host()} | {_currentSystem}" +
            (mapped != null ? "" : " (无缩略图映射)") +
            $" | 显示 {shown}/{_rows.Count} | ROM {local} | 图 {_rows.Count(r => r.HasImage)} | 视频 {_rows.Count(r => r.HasVideo)}"
            + (_dirty ? " | *未保存" : "");
    }

    // ---- FTP ----

    private async Task<string> FtpDownloadStringAsync(string remotePath)
    {
        var req = MakeFtpRequest(remotePath, WebRequestMethods.Ftp.DownloadFile);
        using var resp = (FtpWebResponse)await req.GetResponseAsync();
        await using var stream = resp.GetResponseStream()!;
        using var reader = new StreamReader(stream, Encoding.UTF8);
        return await reader.ReadToEndAsync();
    }

    private async Task FtpDownloadFileAsync(string remotePath, string localPath)
    {
        var req = MakeFtpRequest(remotePath, WebRequestMethods.Ftp.DownloadFile);
        using var resp = (FtpWebResponse)await req.GetResponseAsync();
        await using var stream = resp.GetResponseStream()!;
        await using var fs = File.Create(localPath);
        await stream.CopyToAsync(fs);
    }

    private async Task FtpUploadStringAsync(string remotePath, string content) =>
        await FtpUploadBytesAsync(remotePath, new UTF8Encoding(false).GetBytes(content));

    private async Task FtpUploadBytesAsync(string remotePath, byte[] bytes)
    {
        // Retry: MiSTer FTP occasionally drops concurrent/idle connections.
        Exception? last = null;
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                var req = MakeFtpRequest(remotePath, WebRequestMethods.Ftp.UploadFile);
                req.ContentLength = bytes.Length;
                await using (var stream = await req.GetRequestStreamAsync())
                {
                    await stream.WriteAsync(bytes);
                    await stream.FlushAsync();
                }
                using var resp = (FtpWebResponse)await req.GetResponseAsync();
                if (resp.StatusCode is FtpStatusCode.ClosingData
                    or FtpStatusCode.FileActionOK
                    or FtpStatusCode.CommandOK)
                    return;
                last = new IOException($"FTP 状态异常: {resp.StatusCode} {resp.StatusDescription}");
            }
            catch (Exception ex)
            {
                last = ex;
                await Task.Delay(200 * (attempt + 1));
            }
        }
        throw last ?? new IOException("FTP 上传失败");
    }

    private async Task<byte[]> FtpDownloadBytesAsync(string remotePath)
    {
        var req = MakeFtpRequest(remotePath, WebRequestMethods.Ftp.DownloadFile);
        using var resp = (FtpWebResponse)await req.GetResponseAsync();
        await using var stream = resp.GetResponseStream()!;
        using var ms = new MemoryStream();
        await stream.CopyToAsync(ms);
        return ms.ToArray();
    }

    private async Task<long?> FtpSizeAsync(string remotePath)
    {
        try
        {
            var req = MakeFtpRequest(remotePath, WebRequestMethods.Ftp.GetFileSize);
            using var resp = (FtpWebResponse)await req.GetResponseAsync();
            return resp.ContentLength;
        }
        catch
        {
            return null;
        }
    }

    private async Task FtpEnsureDirAsync(string remoteDir)
    {
        remoteDir = remoteDir.Replace('\\', '/').TrimEnd('/');
        // Create each path segment so /a/b/c works even if only /a exists.
        var parts = remoteDir.Split('/', StringSplitOptions.RemoveEmptyEntries);
        var cur = "";
        foreach (var part in parts)
        {
            cur += "/" + part;
            try
            {
                var list = MakeFtpRequest(cur + "/", WebRequestMethods.Ftp.ListDirectory);
                using var resp = (FtpWebResponse)await list.GetResponseAsync();
                continue;
            }
            catch
            {
                // not present → mkdir
            }
            try
            {
                var mk = MakeFtpRequest(cur, WebRequestMethods.Ftp.MakeDirectory);
                using var resp = (FtpWebResponse)await mk.GetResponseAsync();
                _ = resp.StatusDescription;
            }
            catch
            {
                // race / already exists
            }
        }
    }

    private async Task<List<string>> FtpListNamesAsync(string remoteDir)
    {
        var req = MakeFtpRequest(remoteDir, WebRequestMethods.Ftp.ListDirectory);
        using var resp = (FtpWebResponse)await req.GetResponseAsync();
        await using var stream = resp.GetResponseStream()!;
        using var reader = new StreamReader(stream, Encoding.UTF8);
        var text = await reader.ReadToEndAsync();
        return text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(NormalizeName)
            .Where(s => s.Length > 0)
            .ToList();
    }

    private async Task<List<string>> FtpListFilesAsync(string remoteDir)
    {
        try { return await FtpListNamesAsync(remoteDir); }
        catch { return new List<string>(); }
    }

    private FtpWebRequest MakeFtpRequest(string remotePath, string method)
    {
        remotePath = remotePath.Replace('\\', '/').Trim();
        while (remotePath.StartsWith('/')) remotePath = remotePath[1..];
        // Encode each segment so spaces / parentheses in ROM names upload reliably.
        var encoded = string.Join('/',
            remotePath.Split('/', StringSplitOptions.RemoveEmptyEntries)
                .Select(Uri.EscapeDataString));
        var uri = new Uri($"ftp://{Host()}//{encoded}");
#pragma warning disable SYSLIB0014
        var req = (FtpWebRequest)WebRequest.Create(uri);
#pragma warning restore SYSLIB0014
        req.Method = method;
        req.Credentials = Creds();
        req.EnableSsl = false;
        req.UseBinary = true;
        req.UsePassive = true;
        req.KeepAlive = false;
        req.Timeout = 60000;
        req.ReadWriteTimeout = 120000;
        return req;
    }

    private static string NormalizeName(string raw)
    {
        raw = raw.Trim().TrimEnd('/');
        if (raw.Contains(' ') && (raw.StartsWith('d') || raw.StartsWith('-')))
        {
            var parts = raw.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 9) return parts[^1];
        }
        var slash = raw.LastIndexOf('/');
        return slash >= 0 ? raw[(slash + 1)..] : raw;
    }

    private static HashSet<string> ToBaseNameSet(IEnumerable<string> files, string[] exts)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var f in files)
        {
            var ext = Path.GetExtension(f);
            if (!exts.Any(e => e.Equals(ext, StringComparison.OrdinalIgnoreCase))) continue;
            set.Add(Path.GetFileNameWithoutExtension(f));
        }
        return set;
    }

    private static string XmlEscape(string s) =>
        s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace("\"", "&quot;");

    private sealed class GameRow
    {
        public string FileName { get; set; } = "";
        public string DisplayName { get; set; } = "";
        public bool HasRom { get; set; }
        public bool HasImage { get; set; }
        public bool HasVideo { get; set; }
    }
}
