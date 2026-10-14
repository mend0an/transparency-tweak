// Double-click front end for install.ps1 / uninstall.ps1.
// Compiled twice by native\build.ps1: with /define:INSTALL (scripts + DLL + injector embedded)
// and without (uninstall script embedded). The embedded files are extracted to a temp folder,
// the script runs hidden, and the result is shown in a message box.

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class Setup
{
#if INSTALL
#if AERO_PACKAGE
    const string Title = "Install Transparency Tweak Aero " + AeroPackageInfo.Version;
    const string Prompt =
        "This installs the Aero preview in its own version folder and adds glass controls in Personalization > Colors.\n\n" +
        "You do not need to uninstall the previous version. Its program files are kept; only one version is active. " +
        "The previous startup task and settings are saved for Restore Previous Version.\n\n" +
        "Explorer restarts and File Explorer / Settings windows using the tweak may close. Continue?";
#else
    const string Title = "Install Transparency Tweak";
    const string Prompt =
        "This adds a \"Taskbar transparency\" slider to Settings > Personalization > Colors.\n\n" +
        "No administrator rights are needed and nothing in the Windows system folders is changed. " +
        "If it is already installed, it is updated. Updating an active older version restarts Explorer " +
        "and closes File Explorer and Settings windows using that version.\n\nContinue?";
#endif
    const string Busy = "Installing...";
#if AERO_PACKAGE
    static readonly string[] Files = { "install.ps1", "switch.ps1", "package.json", @"native\out\TweakTap.dll", @"native\out\TransparencyTweak.exe" };
#else
    static readonly string[] Files = { "install.ps1", @"native\out\TweakTap.dll", @"native\out\TransparencyTweak.exe" };
#endif
    const string Script = "install.ps1";
#else
#if AERO_PACKAGE
    const string Title = "Restore Previous Transparency Tweak Version";
    const string Prompt =
        "This switches from Aero " + AeroPackageInfo.Version + " to the version and settings saved before its installation.\n\n" +
        "The Aero package files are kept. Explorer restarts, so File Explorer / Settings windows using the tweak may close. Continue?";
    const string Busy = "Restoring previous version...";
    static readonly string[] Files = { "uninstall.ps1", "switch.ps1", "package.json" };
#else
    const string Title = "Uninstall Transparency Tweak";
    const string Prompt =
        "This removes Transparency Tweak and returns the taskbar to the Windows default.\n\n" +
        "Explorer restarts, so open File Explorer windows will close (other apps are not affected).\n\nContinue?";
    const string Busy = "Uninstalling...";
    static readonly string[] Files = { "uninstall.ps1" };
#endif
    const string Script = "uninstall.ps1";
#endif

    [STAThread]
    static int Main()
    {
        Application.EnableVisualStyles();
        if (MessageBox.Show(Prompt, Title, MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK)
            return 1;

        string dir = Path.Combine(Path.GetTempPath(), "TransparencyTweakSetup-" + Guid.NewGuid().ToString("N"));
        int exitCode = -1;
        string output = "";
        try
        {
            Extract(dir);
            RunWithProgress(() => exitCode = RunScript(Path.Combine(dir, Script), out output));
        }
        catch (Exception ex)
        {
            output = ex.Message;
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        if (exitCode == 0)
            MessageBox.Show(output.Trim(), Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
        else
            MessageBox.Show("Something went wrong:\n\n" + output.Trim(), Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
        return exitCode == 0 ? 0 : 2;
    }

    static void Extract(string dir)
    {
        var assembly = Assembly.GetExecutingAssembly();
        foreach (string file in Files)
        {
            string target = Path.Combine(dir, file);
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            using (Stream source = assembly.GetManifestResourceStream(file.Replace('\\', '/')))
            using (FileStream sink = File.Create(target))
                source.CopyTo(sink);
        }
    }

    static int RunScript(string script, out string output)
    {
        var psi = new ProcessStartInfo("powershell.exe",
            "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\"");
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;

        var text = new StringBuilder();
        using (var process = Process.Start(psi))
        {
            process.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (text) text.AppendLine(e.Data); };
            process.BeginErrorReadLine();
            string stdout = process.StandardOutput.ReadToEnd();
            process.WaitForExit();
            lock (text) text.Insert(0, stdout);
            output = text.ToString();
            return process.ExitCode;
        }
    }

    // A small "please wait" window while the script runs (uninstall takes a few seconds).
    static void RunWithProgress(Action work)
    {
        var form = new Form();
        form.Text = Title;
        form.FormBorderStyle = FormBorderStyle.FixedDialog;
        form.ControlBox = false;
        form.StartPosition = FormStartPosition.CenterScreen;
        form.ClientSize = new Size(320, 90);
        form.ShowInTaskbar = true;
        var label = new Label { Text = Busy, AutoSize = true, Location = new Point(16, 16) };
        var bar = new ProgressBar { Style = ProgressBarStyle.Marquee, Location = new Point(16, 46), Size = new Size(288, 18) };
        form.Controls.Add(label);
        form.Controls.Add(bar);

        Exception failure = null;
        form.Shown += (s, e) =>
        {
            var worker = new Thread(() =>
            {
                try { work(); }
                catch (Exception ex) { failure = ex; }
                form.BeginInvoke(new Action(form.Close));
            });
            worker.IsBackground = true;
            worker.Start();
        };
        Application.Run(form);
        if (failure != null) throw failure;
    }
}
