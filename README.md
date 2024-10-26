# Discord Canary Launcher
This is a wrapper around discord canary to make it update itself instead of relying on a package manager.

To give yourself a seamless experience copy the discord-canary.desktop file from `/usr/share/applications` to `~/.local/share/applications` and replace the `Exec=(Your distro's install prefix)/DiscordCanary` with `Exec=/local/usr/bin/discord-canary-launcher`.

To force an update use the `-forceupdate` argument.
To dump the compiled configuration use the `-dumpconfig` argument.
