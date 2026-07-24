using System;
using System.Drawing;
using System.Windows;
using Forms = System.Windows.Forms;

namespace X3LaptopCompanion
{
    public sealed class TrayController : IDisposable
    {
        private readonly MainWindow mainWindow;
        private readonly Forms.NotifyIcon notifyIcon;

        public TrayController(MainWindow mainWindow)
        {
            this.mainWindow = mainWindow;
            notifyIcon = new Forms.NotifyIcon
            {
                Icon = SystemIcons.Application,
                Text = "X3 Laptop Companion",
                Visible = true,
                ContextMenuStrip = BuildMenu()
            };
            notifyIcon.DoubleClick += (sender, args) => ShowStatus();
        }

        public void ShowStatus()
        {
            mainWindow.Show();
            if (mainWindow.WindowState == WindowState.Minimized)
            {
                mainWindow.WindowState = WindowState.Normal;
            }

            mainWindow.Activate();
        }

        public void Dispose()
        {
            notifyIcon.Visible = false;
            notifyIcon.Dispose();
        }

        private Forms.ContextMenuStrip BuildMenu()
        {
            var menu = new Forms.ContextMenuStrip();
            menu.Items.Add("Show Status", null, (sender, args) => ShowStatus());
            menu.Items.Add("Toggle Teams Mute", null, (sender, args) => mainWindow.ToggleMuteFromUi());
            menu.Items.Add("Open Log", null, (sender, args) => mainWindow.OpenLog());
            menu.Items.Add(new Forms.ToolStripSeparator());
            menu.Items.Add("Exit", null, (sender, args) => Application.Current.Shutdown());
            return menu;
        }
    }
}
