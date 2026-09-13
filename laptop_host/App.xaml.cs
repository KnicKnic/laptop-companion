using System;
using System.Windows;

namespace X3LaptopCompanion
{
    public partial class App : Application
    {
        private TrayController trayController;
        private MainWindow mainWindow;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            mainWindow = new MainWindow();
            Console.CancelKeyPress += OnConsoleCancelKeyPress;
            trayController = new TrayController(mainWindow);
            trayController.ShowStatus();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            Console.CancelKeyPress -= OnConsoleCancelKeyPress;
            if (trayController != null)
            {
                trayController.Dispose();
            }

            base.OnExit(e);
        }

        private void OnConsoleCancelKeyPress(object sender, ConsoleCancelEventArgs e)
        {
            e.Cancel = true;
            // Console signals arrive off the WPF thread; use the same cleanup as tray Exit.
            Dispatcher.BeginInvoke(new Action(() =>
            {
                HostLog.Write("Console " + e.SpecialKey + " received; requesting app exit.");
                mainWindow.ExitApplication();
            }));
        }
    }
}
