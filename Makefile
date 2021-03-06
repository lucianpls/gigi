INCLUDES = -I $(HOME)/include
TARGET = gigi
INPUTS = $(TARGET).cpp
HEADERS =
LDFLAGS = -L$(HOME)/lib
CXXFLAGS = -O2 
LIBS = -lfcgi -lgdal -lfcgi++ -lcgicc

$(TARGET): $(INPUTS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(INPUTS) $(LDFLAGS) $(LIBS)

install: $(TARGET)
	sudo service httpd stop
	cp $^ /var/www/html
	sudo service httpd start

clean:
	$(RM) $(TARGET)
