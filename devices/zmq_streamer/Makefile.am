INCLUDES = -I$(top_srcdir)/bindings/cpp -I$(top_srcdir)/bindings/c

bin_PROGRAMS = zmq_streamer

zmq_streamer_LDADD = $(top_builddir)/src/libzmq.la
zmq_streamer_SOURCES = zmq_streamer.cpp
zmq_streamer_CXXFLAGS = -Wall -pedantic -Werror


