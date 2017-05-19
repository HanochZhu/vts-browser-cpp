#ifndef GPUCONTEXT_H_awfvgbhjk
#define GPUCONTEXT_H_awfvgbhjk

#include <string>
#include <vector>
#include <glad/glad.h>
#include <vts-browser/resources.hpp>

extern bool anisotropicFilteringAvailable;
extern bool openglDebugAvailable;

void checkGl(const char *name = nullptr);

void initializeGpuContext();

class GpuShaderImpl
{
public:
    GLuint id;

    GpuShaderImpl();
    ~GpuShaderImpl();
    void clear();
    void bind();
    int loadShader(const std::string &source, int stage);
    void loadShaders(const std::string &vertexShader,
                     const std::string &fragmentShader);
    void uniformMat4(vts::uint32 location, const float *value);
    void uniformMat3(vts::uint32 location, const float *value);
    void uniformVec4(vts::uint32 location, const float *value);
    void uniformVec3(vts::uint32 location, const float *value);
    void uniform(vts::uint32 location, const float value);
    void uniform(vts::uint32 location, const int value);
    
    std::vector<vts::uint32> uniformLocations;
};

class GpuTextureImpl
{
public:
    GLuint id;
    bool grayscale;

    static GLenum findInternalFormat(const vts::GpuTextureSpec &spec);
    static GLenum findFormat(const vts::GpuTextureSpec &spec);
    
    GpuTextureImpl();
    ~GpuTextureImpl();
    void clear();
    void bind();
    void loadTexture(vts::ResourceInfo &info, const vts::GpuTextureSpec &spec);
};

class GpuMeshImpl
{
public:
    vts::GpuMeshSpec spec;
    GLuint vao, vbo, vio;

    GpuMeshImpl();
    ~GpuMeshImpl();
    void clear();
    void bind();
    void dispatch();
    void loadMesh(vts::ResourceInfo &info, const vts::GpuMeshSpec &spec);
};

#endif
