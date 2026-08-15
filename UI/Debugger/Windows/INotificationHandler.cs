using Mesen.Interop;

namespace Mesen.Debugger.Windows
{
	public interface INotificationHandler
	{
		bool ShouldProcessNotification(NotificationEventArgs e) { return true; }
		void ProcessNotification(NotificationEventArgs e);
	}
}
