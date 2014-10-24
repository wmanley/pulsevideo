PKGS=--pkg gstreamer-1.0 --pkg gio-2.0 --pkg posix --pkg gio-unix-2.0

main: main.vala Makefile
	valac $(PKGS) main.vala

client.c : client.vala Makefile
	valac -C $(PKGS) $<

client: client.vala Makefile
	valac $(PKGS) client.vala

