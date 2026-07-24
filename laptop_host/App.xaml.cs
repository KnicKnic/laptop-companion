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
            trayController = new TrayController(mainWindow);
            trayController.ShowStatus();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            if (trayController != null)
            {
                trayController.Dispose();
            }

            base.OnExit(e);
        }
    }
}
