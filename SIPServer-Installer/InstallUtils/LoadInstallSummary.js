// Immediate UI custom action (Product.wxs, LoadInstallSummary): reads the installation summary written by
// Configure-SIPServerConfig.ps1 (UTF-16) into INSTALL_SUMMARY for InstallSummaryDlg. Never fails the installation.
try {
    var root = Session.Property("AppRoot");
    if (root && root.charAt(root.length - 1) != "\\") root += "\\";
    var p = root + "SIPServerConfigBackup\\InstallSummary.txt";
    var fso = new ActiveXObject("Scripting.FileSystemObject");
    if (fso.FileExists(p)) {
        var f = fso.OpenTextFile(p, 1, false, -1);
        var t = f.AtEndOfStream ? "" : f.ReadAll();
        f.Close();
        Session.Property("INSTALL_SUMMARY") = t;
    }
} catch (e) { }
