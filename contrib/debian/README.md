
Debian
====================
This directory contains files used to package safenoded/safenode-qt
for Debian-based Linux systems. If you compile safenoded/safenode-qt yourself, there are some useful files here.

## safenode: URI support ##


safenode-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install safenode-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your safenode-qt binary to `/usr/bin`
and the `../../share/pixmaps/safenode128.png` to `/usr/share/pixmaps`

safenode-qt.protocol (KDE)

