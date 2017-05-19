#include <unistd.h> // usleep
#include <vts-browser/map.hpp>
#include "dataThread.hpp"
#include "threadName.hpp"
#include "gpuContext.hpp"
#include <GLFW/glfw3.h>

namespace
{
    void run(DataThread *data)
    {
        data->run();
    }
}

DataThread::DataThread(GLFWwindow *shared) : map(nullptr),
    window(nullptr), stop(false)
{
    fetcher = vts::Fetcher::create(vts::FetcherOptions());
    window = glfwCreateWindow(1, 1, "data context", NULL, shared);
    glfwSetWindowUserPointer(window, this);
    glfwHideWindow(window);
    initializeGpuContext();
    thr = std::thread(&::run, this);
}

DataThread::~DataThread()
{
    stop = true;
    thr.join();
    glfwDestroyWindow(window);
    window = nullptr;
}

void DataThread::run()
{
    setThreadName("data");
    glfwMakeContextCurrent(window);
    while (!stop && !map)
        usleep(1000);
    setThreadName("downloader"); // the downloader threads inherits the name
    map->dataInitialize(fetcher);
    setThreadName("data");
    while (!stop)
    {
        if (map->dataTick())
            usleep(20000);
        else
            usleep(5000);
    }
    map->dataFinalize();
}
