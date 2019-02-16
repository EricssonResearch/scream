# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgstgscreamtx.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

## Plugin 1

# sources used to compile this plug-in
libgstgscreamtx_la_SOURCES = gstgscreamtx.cpp gstgscreamtx.h ScreamTx.h TestFile.h ScreamTx.cpp RtpQueue.cpp

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstgscreamtx_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_CFLAGS)
libgstgscreamtx_la_CXXFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_CXXFLAGS)
libgstgscreamtx_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) -lgstvideo-1.0 $(GST_LIBS) $(OPENH264_LIBS)
libgstgscreamtx_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstgscreamtx_la_LIBTOOLFLAGS = --tag=disable-static
INCLUDES=-I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/

# headers we need but don't want installed
noinst_HEADERS = gstgscreamtx.h ScreamTx.h
