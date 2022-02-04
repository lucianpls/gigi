INCLUDES = -I $(HOME)/include
TARGET = gigi
INPUTS = $(TARGET).cpp
HEADERS =
LDFLAGS = -L$(HOME)/lib
CXXFLAGS = -O2 
LIBS = -lfcgi -lgdal -lfcgi++ -lcgicc -llua

$(TARGET): $(INPUTS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(INPUTS) $(LDFLAGS) $(LIBS)

install: $(TARGET)
	sudo service httpd stop
	cp gigi.config /var/www/cgi-bin
	cp wi.wms /var/www/cgi-bin
	cp $^ /var/www/cgi-bin
	sudo service httpd start

clean:
	$(RM) $(TARGET)

test: $(TARGET)
	rm -f out.*
	QUERY_STRING="What&size=1024,1024&ID=0230123101310331&RAW=1&bbox=0,0,4096,4096" ./$(TARGET) >out.jpg
	# QUERY_STRING="What&size=1024,1024&RAW=1" ./$(TARGET) >out.jpg
	gdalinfo -hist out.jpg
