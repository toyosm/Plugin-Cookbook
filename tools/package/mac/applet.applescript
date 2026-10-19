-- Plugin Studio for macOS: a stay-open applet, so the app has a Dock icon and a Quit menu.
-- launch.sh does the work (first-run setup, then the Studio server); quitting the app stops the server.

property serverPID : ""

on launcherPath()
	return quoted form of (POSIX path of (path to me) & "Contents/Resources/launch.sh")
end launcherPath

on run
	set serverPID to do shell script launcherPath() & " start"
end run

-- clicking the Dock icon again reopens the Studio page
on reopen
	do shell script launcherPath() & " open"
end reopen

on quit
	if serverPID is not "" then
		do shell script "kill " & serverPID & " >/dev/null 2>&1 || true"
	end if
	continue quit
end quit
