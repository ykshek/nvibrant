#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <fcntl.h>
#include <sys/ioctl.h>

// Open GPU Kernel Modules
#include "nvkms-ioctl.h"
#include "nvkms-api.h"

// New CLI
#include <getopt.h>

// ------------------------------------------------------------------------------------------------|

// NvKms ioctl calls must match the driver version
const char* NVIDIA_DRIVER_VERSION = []() {

    // Safety fallback or override with environment variable
    if (const char* version = getenv("NVIDIA_DRIVER_VERSION_CPP"))
        return version;

    // Seems to be a common and stable path to get the information
    if (auto file = std::ifstream("/sys/module/nvidia/version")) {
        static std::string version;
        std::getline(file, version);
        return version.c_str();
    }

    printf("Could not find the current driver version from /sys/module/nvidia/version\n");
    printf("• Run with 'NVIDIA_DRIVER_VERSION_CPP=x.y.z nvibrant' to set or force it\n");
    exit(1);
}();

// Nth GPU index to allocate device handle
const NvU32 NVIDIA_GPU = []() {
    if (const char* num = getenv("NVIDIA_GPU"))
        return atoi(num);
    return 0;
}();

// What attribute to set on the displays
const char* ATTRIBUTE = []() {
    if (const char* name = getenv("ATTRIBUTE"))
        return name;
    return "vibrance";
}();

// Wrapper to populate a NvKmsIoctlParams and call ioctl for generic types
template <typename T> int easy_nvkms_ioctl(int fd, NvU32 cmd, T* data) {
    NvKmsIoctlParams params;
    params.cmd     = cmd;
    params.size    = sizeof(T);
    params.address = (NvU64) data;
    return ioctl(fd, NVKMS_IOCTL_IOWR, &params);
}

// Smart parse, limit, safe default an integer from argv at a given index
int get_int(int argc, char* argv[], int index, int min, int max, int fallback) {
    return std::max(min, std::min(max, (index < argc) ? atoi(argv[index]) : fallback));
}

void usage(void)
{
    printf( "Usage: nvibrant [VIBRANCE] [OPTION]...\n"
    "\t-h, --help\t\t\t Display this help\n"
    "\t-d [ID], --display=[ID]\t\t Specify which display to set. Set all if unspecified\n"
    "\t-l, --list\t\t\t List all vibrance settings of all ports without setting"
    "\n"
    "The ID argument is an integer specifying which display to set.\n"
    "If unset or invalid, defaults to setting all connected displays.\n"
    "\n"
    "Exit status:\n"
    "0\tif OK,\n"
    "1\tif problem occurs.\n"
    );
    return;
}

// ------------------------------------------------------------------------------------------------|

int main(int argc, char **argv) {
    printf("Driver version: (%s)\n", NVIDIA_DRIVER_VERSION);

    int opt;
    int display_id = -1;    // -1 means set all valid monitors, -2 means none.


    const char *options = "hdl";
    struct option long_options[] = {
        {"help",    0,      NULL,   'h'},
        {"display", 0,      NULL,   'd'},
        {"list",    0,      NULL,   'l'},
        {NULL,      0,      NULL,   0}
    };

    while (true) {
        opt = getopt_long(argc, argv, options, long_options, NULL);
        if (opt == -1)
            break;
        switch(opt) {
            case 'h':
                usage();
                break;
            case 'd':
                display_id = atoi(optarg);
                break;
            case 'l':
                display_id = -2;
                // skip command line parsing with display_id = -2
                break;
            case '?':
                printf("Unknown option '%c' (decimal: %d)\n", optopt, optopt);
                usage();
                exit(0);
                break;
            default:
                printf("Error at '%c' (decimal: %d)\n", optopt, optopt);
                exit(1);
                break;
        }
    }
    int vibrance = 0;
    int dithering = 2;
    int nargs = argc - optind?optind:1;
    char *endptr;
    if (argv[nargs])
        vibrance = strtol(argv[nargs], &endptr, 10);
    vibrance = (*endptr != '\0' || !display_id) && !(vibrance >= -1023 || vibrance <= 1024)? 0: vibrance;
    if (argv[nargs + 1])
        dithering = strtol(argv[nargs], &endptr, 10);
    dithering = (*endptr != '\0' || !display_id) && !(dithering >= 0 || dithering <= 2)? 2: dithering;

    printf("Setting Vibrance %d for display %d.\n", vibrance, display_id);

    // Open the nvidia-modeset file descriptor
    int modeset = open("/dev/nvidia-modeset", O_RDWR);
    if (modeset < 0) {
        printf("Failed to open /dev/nvidia-modeset device file for ioctl calls\n");
        printf("• Perhaps 'nvidia_drm.modeset=1' kernel parameter is missing?\n");
        return 1;
    }

    // Initialize nvkms to get a deviceHandle, dispHandle, etc.
    struct NvKmsAllocDeviceParams allocDevice;
    memset(&allocDevice, 0, sizeof(allocDevice));
    memcpy(&allocDevice.request.deviceId, &NVIDIA_GPU, sizeof(NvU32));
    strcpy(allocDevice.request.versionString, NVIDIA_DRIVER_VERSION);
    allocDevice.request.sliMosaic = NV_FALSE;
    allocDevice.request.tryInferSliMosaicFromExistingDevice = NV_FALSE;
    allocDevice.request.no3d = NV_TRUE;
    allocDevice.request.enableConsoleHotplugHandling = NV_FALSE;
    if (easy_nvkms_ioctl(modeset, NVKMS_IOCTL_ALLOC_DEVICE, &allocDevice) < 0) {
        switch (allocDevice.reply.status) {
            case NVKMS_ALLOC_DEVICE_STATUS_VERSION_MISMATCH:
                printf("Driver version mismatch, maybe reboot?\n");
                return 1;
            default:
                printf("AllocDevice ioctl failed\n");
                return 1;
        }
    }

    // Current Nth target monitor
    int index = 0;

    // Iterate on all displays in the system, querying their info
    for (NvU32 display=0; display<allocDevice.reply.numDisps; display++) {
        printf("\nDisplay %d:\n", display);
        NvKmsQueryDispParams queryDisp;
        queryDisp.request.deviceHandle = allocDevice.reply.deviceHandle;
        queryDisp.request.dispHandle   = allocDevice.reply.dispHandles[display];
        if (easy_nvkms_ioctl(modeset, NVKMS_IOCTL_QUERY_DISP, &queryDisp) < 0) {
            printf(" QueryDisp ioctl failed\n");
            continue;
        }

        // Iterate on all physical connections of the GPU (literally, hdmi, dp, etc.)
        for (NvU32 connector=0; connector<queryDisp.reply.numConnectors; connector++) {
            index++;

            // Get 'immutable' static data (such as connector type)
            NvKmsQueryConnectorStaticDataParams staticData;
            staticData.request.deviceHandle    = allocDevice.reply.deviceHandle;
            staticData.request.dispHandle      = allocDevice.reply.dispHandles[display];
            staticData.request.connectorHandle = queryDisp.reply.connectorHandles[connector];
            if (easy_nvkms_ioctl(modeset, NVKMS_IOCTL_QUERY_CONNECTOR_STATIC_DATA, &staticData) < 0) {
                printf("QueryConnector ioctl failed\n");
                continue;
            }

            // Get the 'dynamic' data (is connected?)
            NvKmsQueryDpyDynamicDataParams dynamicData;
            dynamicData.request.deviceHandle    = allocDevice.reply.deviceHandle;
            dynamicData.request.dispHandle      = allocDevice.reply.dispHandles[display];
            dynamicData.request.dpyId           = staticData.reply.dpyId;
            if (easy_nvkms_ioctl(modeset, NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &dynamicData) < 0) {
                printf("QueryDpy ioctl failed\n");
                continue;
            }

            // Print basic display info
            printf("• (%d, ", connector);
            switch (staticData.reply.type) {
                case NVKMS_CONNECTOR_TYPE_DP:    printf("DP  "); break;
                case NVKMS_CONNECTOR_TYPE_VGA:   printf("VGA "); break;
                case NVKMS_CONNECTOR_TYPE_DVI_I: printf("DVII"); break;
                case NVKMS_CONNECTOR_TYPE_DVI_D: printf("DVID"); break;
                case NVKMS_CONNECTOR_TYPE_ADC:   printf("ADC "); break;
                case NVKMS_CONNECTOR_TYPE_LVDS:  printf("LVDS"); break;
                case NVKMS_CONNECTOR_TYPE_HDMI:  printf("HDMI"); break;
                case NVKMS_CONNECTOR_TYPE_USBC:  printf("USBC"); break;
                case NVKMS_CONNECTOR_TYPE_DSI:   printf("DSI "); break;
                default: break;
            }
            printf(") • ");

            // Make the request to set attribute for this monitor
            NvKmsSetDpyAttributeParams setDpyAttr;
            setDpyAttr.request.deviceHandle = allocDevice.reply.deviceHandle;
            setDpyAttr.request.dispHandle   = allocDevice.reply.dispHandles[display];
            setDpyAttr.request.dpyId        = staticData.reply.dpyId;

            // Branch on what display attribute to set
            if (strcmp(ATTRIBUTE, "vibrance") == 0 && (index == display_id || display_id == -2)) {
                setDpyAttr.request.attribute = NV_KMS_DPY_ATTRIBUTE_DIGITAL_VIBRANCE;
                setDpyAttr.request.value     = vibrance;
            } else if (strcmp(ATTRIBUTE, "dithering") == 0 && (index == display_id || display_id == -2)) {
                setDpyAttr.request.attribute = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING;
                setDpyAttr.request.value     = dithering;
            } else {
                printf("Unknown attribute '%s' to set\n", ATTRIBUTE);
                continue;
            }
            printf("Set %s (%5lld) • ", ATTRIBUTE, setDpyAttr.request.value);

            // Can't set attributes on disconnected outputs
            if (!dynamicData.reply.connected) {
                printf("None\n");
                continue;
            }

            if (easy_nvkms_ioctl(modeset, NVKMS_IOCTL_SET_DPY_ATTRIBUTE, &setDpyAttr) < 0) {
                printf("Failed\n");
                continue;
            }
            printf("Success\n");
        }
    }

    // Deallocate device just in case it's not gc-ed on the driver side
    struct NvKmsFreeDeviceParams freeDevice;
    freeDevice.request.deviceHandle = allocDevice.reply.deviceHandle;
    if (easy_nvkms_ioctl(modeset, NVKMS_IOCTL_FREE_DEVICE, &freeDevice) < 0) {
        printf("Failed to free device handle\n");
        return 1;
    }

    return 0;
}
