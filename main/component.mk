#
# Component Makefile
#
# This Makefile should, at the very least, just include $(SDK_PATH)/Makefile. By default,
# this will take the sources in the src/ directory, compile them and link them into
# lib(subdirectory_name).a in the build directory. This behaviour is entirely configurable,
# please read the SDK documents if you need to do this.
#
#CFLAGS += -D LOG_LOCAL_LEVEL=ESP_LOG_DEBUG
CFLAGS += -D LOG_LOCAL_LEVEL=ESP_LOG_INFO -DMODEL_NAME=SqueezeESP32
COMPONENT_ADD_INCLUDEDIRS += 	$(COMPONENT_PATH)/../tools	\
								$(COMPONENT_PATH)/../config
COMPONENT_EXTRA_INCLUDES += $(PROJECT_PATH)/components/tools/
LDFLAGS += -s
COMPONENT_EMBED_TXTFILES :=  ${PROJECT_PATH}/server_certs/github.pem