// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "VideoBackends/OGL/FramebufferManager.h"
#include "VideoBackends/OGL/GLUtil.h"
#include "VideoBackends/OGL/PostProcessing.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{

static const char s_vertex_workaround_shader[] =
	"in vec4 rawpos;\n"
	"out vec2 uv0;\n"
	"uniform vec4 src_rect;\n"
	"void main(void) {\n"
	"	gl_Position = vec4(rawpos.xy, 0.0, 1.0);\n"
	"	uv0 = rawpos.zw * src_rect.zw + src_rect.xy;\n"
	"}\n";

static const char s_vertex_shader[] =
	"out vec2 uv0;\n"
	"uniform vec4 src_rect;\n"
	"void main(void) {\n"
	"	vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);\n"
	"	gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);\n"
	"	uv0 = rawpos * src_rect.zw + src_rect.xy;\n"
	"}\n";

OpenGLPostProcessing::OpenGLPostProcessing()
	: m_initialized(false), m_prev_height(0), m_prev_width(0)
{
	CreateHeader();

	m_attribute_workaround = DriverDetails::HasBug(DriverDetails::BUG_BROKENATTRIBUTELESS);
	if (m_attribute_workaround)
	{
		glGenBuffers(1, &m_attribute_vbo);
		glGenVertexArrays(1, &m_attribute_vao);
	}
}

void OpenGLPostProcessing::DestroyStageOutput()
{
	for (auto& tex : m_stageOutput)
	{
		if (tex.first)
		{
			glDeleteTextures(1, &tex.first);
			tex.first = 0;
		}

		if (tex.second)
		{
			glDeleteFramebuffers(1, &tex.second);
			tex.second = 0;
		}
	}	
}

OpenGLPostProcessing::~OpenGLPostProcessing()
{
	DestroyStageOutput();
	for (auto& shader : m_shaders)
	{
		shader.shader.Destroy();
	}
	if (m_attribute_workaround)
	{
		glDeleteBuffers(1, &m_attribute_vbo);
		glDeleteVertexArrays(1, &m_attribute_vao);
	}
}

void OpenGLPostProcessing::BlitFromTexture(const TargetRectangle &src, const TargetRectangle &dst,
	void* src_texture_ptr, void* src_depth_texture_ptr, int src_width, int src_height, int layer, float gamma)
{
	int src_texture = *((int*)src_texture_ptr);
	int src_texture_depth = *((int*)src_depth_texture_ptr);
	ApplyShader();

	if (m_attribute_workaround)
		glBindVertexArray(m_attribute_vao);
	else
		OpenGL_BindAttributelessVAO();		

	glActiveTexture(GL_TEXTURE0+9);
	glBindTexture(GL_TEXTURE_2D_ARRAY, src_texture);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glActiveTexture(GL_TEXTURE0 + 10);
	glBindTexture(GL_TEXTURE_2D_ARRAY, src_texture_depth);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	const auto& stages = m_config.GetStages();
	size_t finalstage = stages.size() - 1;
	if (finalstage > 0 &&
		(m_prev_width != dst.GetWidth()
		|| m_prev_height != dst.GetHeight()
		|| m_stageOutput.size() != finalstage))
	{
		m_prev_width = dst.GetWidth();
		m_prev_height = dst.GetHeight();
		DestroyStageOutput();
		m_stageOutput.resize(finalstage);
		for (size_t i = 0; i < finalstage; i++)
		{
			u32 stage_width = (u32)(dst.GetWidth() * stages[i].m_outputScale);
			u32 stage_height = (u32)(dst.GetHeight() * stages[i].m_outputScale);
			auto &stage_output = m_stageOutput[i];
			glGenTextures(1, &stage_output.first);
			glActiveTexture(GL_TEXTURE0 + 11);
			glBindTexture(GL_TEXTURE_2D_ARRAY, stage_output.first);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, stage_width, stage_height, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glGenFramebuffers(1, &stage_output.second);
			FramebufferManager::SetFramebuffer(stage_output.second);
			FramebufferManager::FramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_ARRAY, stage_output.first, 0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
		}
	}
	for (size_t i = 0; i < stages.size(); i++)
	{
		if (i == finalstage)
		{
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			glViewport(dst.left, dst.bottom, dst.GetWidth(), dst.GetHeight());
		}
		else
		{
			FramebufferManager::SetFramebuffer(m_stageOutput[i].second);
			glViewport(0, 0, (GLsizei)(dst.GetWidth() * stages[i].m_outputScale), (GLsizei)(dst.GetHeight()* stages[i].m_outputScale));
		}
		ShaderInstance& currentshader = m_shaders[i];
		currentshader.shader.Bind();
		glUniform4f(currentshader.m_uniform_resolution, (float)src_width, (float)src_height, 1.0f / (float)src_width, 1.0f / (float)src_height);
		glUniform4f(currentshader.m_uniform_src_rect, src.left / (float)src_width, src.bottom / (float)src_height,
			src.GetWidth() / (float)src_width, src.GetHeight() / (float)src_height);
		glUniform1ui(currentshader.m_uniform_time, (GLuint)m_timer.GetTimeElapsed());
		glUniform1i(currentshader.m_uniform_layer, layer);
		glUniform1f(currentshader.m_uniform_gamma, gamma);
		if (m_config.IsDirty())
		{
			for (auto& it : m_config.GetOptions())
			{
				if (it.second.m_dirty)
				{
					switch (it.second.m_type)
					{
					case PostProcessingShaderConfiguration::ConfigurationOption::OptionType::OPTION_BOOL:
						glUniform1i(currentshader.m_uniform_bindings[it.first], it.second.m_bool_value);
						break;
					case PostProcessingShaderConfiguration::ConfigurationOption::OptionType::OPTION_INTEGER:
						switch (it.second.m_integer_values.size())
						{
						case 1:
							glUniform1i(currentshader.m_uniform_bindings[it.first], it.second.m_integer_values[0]);
							break;
						case 2:
							glUniform2i(currentshader.m_uniform_bindings[it.first],
								it.second.m_integer_values[0],
								it.second.m_integer_values[1]);
							break;
						case 3:
							glUniform3i(currentshader.m_uniform_bindings[it.first],
								it.second.m_integer_values[0],
								it.second.m_integer_values[1],
								it.second.m_integer_values[2]);
							break;
						case 4:
							glUniform4i(currentshader.m_uniform_bindings[it.first],
								it.second.m_integer_values[0],
								it.second.m_integer_values[1],
								it.second.m_integer_values[2],
								it.second.m_integer_values[3]);
							break;
						}
						break;
					case PostProcessingShaderConfiguration::ConfigurationOption::OptionType::OPTION_FLOAT:
						switch (it.second.m_float_values.size())
						{
						case 1:
							glUniform1f(currentshader.m_uniform_bindings[it.first], it.second.m_float_values[0]);
							break;
						case 2:
							glUniform2f(currentshader.m_uniform_bindings[it.first],
								it.second.m_float_values[0],
								it.second.m_float_values[1]);
							break;
						case 3:
							glUniform3f(currentshader.m_uniform_bindings[it.first],
								it.second.m_float_values[0],
								it.second.m_float_values[1],
								it.second.m_float_values[2]);
							break;
						case 4:
							glUniform4f(currentshader.m_uniform_bindings[it.first],
								it.second.m_float_values[0],
								it.second.m_float_values[1],
								it.second.m_float_values[2],
								it.second.m_float_values[3]);
							break;
						}
						break;
					}					
				}
			}			
		}		
		bool prev_stage_output_required = i > 0 && finalstage > 0;
		if (prev_stage_output_required)
		{
			glActiveTexture(GL_TEXTURE0 + 11);
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_stageOutput[i - 1].first);
		}
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if (prev_stage_output_required)
		{
			glActiveTexture(GL_TEXTURE0 + 11);
			glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		}
	}
	if (m_config.IsDirty())
	{
		for (auto& it : m_config.GetOptions())
		{
			it.second.m_dirty = false;
		}
		m_config.SetDirty(false);
	}
	glActiveTexture(GL_TEXTURE0 + 9);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glActiveTexture(GL_TEXTURE0 + 10);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void OpenGLPostProcessing::ApplyShader()
{
	// shader didn't changed
	if (m_initialized && m_config.GetShader() == g_ActiveConfig.sPostProcessingShader)
		return;
	DestroyStageOutput();
	m_stageOutput.resize(0);
	for (auto& shader : m_shaders)
	{
		shader.shader.Destroy();
	}
	// load shader code
	std::string code = m_config.LoadShader();
	code = LoadShaderOptions(code);

	const char* vertex_shader = s_vertex_shader;

	if (m_attribute_workaround)
		vertex_shader = s_vertex_workaround_shader;

	m_initialized = true;
	const auto& stages = m_config.GetStages();
	m_shaders.resize(stages.size());
	// and compile it
	const char* macros[1];
	for (size_t i = 0; i < stages.size(); i++)
	{
		// and compile it
		std::string entry_point = "#define ";
		entry_point += stages[i].m_stage_entry_point;
		entry_point += " main\n";
		macros[0] = entry_point.c_str();
		if (!ProgramShaderCache::CompileShader(m_shaders[i].shader, vertex_shader, code.c_str(), nullptr, macros, 1))
		{
			ERROR_LOG(VIDEO, "Failed to compile post-processing shader %s", m_config.GetShader().c_str());
			m_initialized = false;
			break;
		}
	}

	// and compile it
	if (!m_initialized)
	{
		g_Config.sPostProcessingShader.clear();
		g_ActiveConfig.sPostProcessingShader.clear();
		for (auto& shader : m_shaders)
		{
			shader.shader.Destroy();
		}
		m_shaders.resize(1);
		code = m_config.LoadShader();
		code = LoadShaderOptions(code);
		ProgramShaderCache::CompileShader(m_shaders[0].shader, vertex_shader, code.c_str());
	}

	for (auto& shader : m_shaders)
	{
		shader.m_uniform_resolution = glGetUniformLocation(shader.shader.glprogid, "resolution");
		shader.m_uniform_gamma = glGetUniformLocation(shader.shader.glprogid, "native_gamma");
		shader.m_uniform_time = glGetUniformLocation(shader.shader.glprogid, "time");
		shader.m_uniform_src_rect = glGetUniformLocation(shader.shader.glprogid, "src_rect");
		shader.m_uniform_layer = glGetUniformLocation(shader.shader.glprogid, "layer");
		shader.m_uniform_bindings.clear();
		for (const auto& it : m_config.GetOptions())
		{
			std::string glsl_name = "option_" + it.first;
			shader.m_uniform_bindings[it.first] = glGetUniformLocation(shader.shader.glprogid, glsl_name.c_str());
		}
	}
	// read uniform locations
	

	if (m_attribute_workaround)
	{
		GLfloat vertices[] = {
			-1.f, -1.f, 0.f, 0.f,
			 1.f, -1.f, 1.f, 0.f,
			-1.f,  1.f, 0.f, 1.f,
			 1.f,  1.f, 1.f, 1.f,
		};

		glBindBuffer(GL_ARRAY_BUFFER, m_attribute_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

		glBindVertexArray(m_attribute_vao);
		glEnableVertexAttribArray(SHADER_POSITION_ATTRIB);
		glVertexAttribPointer(SHADER_POSITION_ATTRIB, 4, GL_FLOAT, 0, 0, nullptr);
	}

	
	m_initialized = true;
}

void OpenGLPostProcessing::CreateHeader()
{
	m_glsl_header = R"GLSL(
// Required variables
// Shouldn't be accessed directly by the PP shader
// Texture sampler
SAMPLER_BINDING(8) uniform sampler2D samp8;
SAMPLER_BINDING(9) uniform sampler2DArray samp9;
SAMPLER_BINDING(10) uniform sampler2DArray samp10;
SAMPLER_BINDING(11) uniform sampler2DArray samp11;

// Output variable
out float4 ocol0;
// Input coordinates
in float2 uv0;
// Resolution
uniform float4 resolution;
// Time
uniform uint time;
// Layer
uniform int layer;
// Gamma
uniform float native_gamma;
// Source Rect
uniform vec4 src_rect;

// Interfacing functions		
float2 GetFragmentCoord()
{
	return gl_FragCoord.xy;
}

float4 Sample(float2 location, int l)
{
	return texture(samp9, float3(location, l));
}

float4 SampleLocationOffset(float2 location, int2 offset)
{ 
	return texture(samp9, float3(location + offset *  resolution.zw, layer));
}

float4 SamplePrev(float2 location)
{
	return texture(samp11, float3((location - src_rect.xy) / src_rect.zw, 0));
}

float4 SamplePrevLocationOffset(float2 location, int2 offset)
{
	float2 newlocation = (location - src_rect.xy) / src_rect.zw;
	newlocation += offset / (src_rect.zw * resolution.xy);
	return texture(samp11, float3(newlocation, 0));
}

float SampleDepth(float2 location, int l)
{
	/*float Znear = 0.001;
	float Zfar = 1.0;
	float A  = (1 - ( Zfar / Znear ))/2;
	float B = (1 + ( Zfar / Znear ))/2;*/
	float A = -499.5;
	float B =  500.5;
	float depth = texture(samp10, float3(location, l)).x;
	depth = 1 / (A * depth + B);
	return depth;
}

float SampleDepthLocationOffset(float2 location, int2 offset)
{
	float A = -499.5;
	float B =  500.5;
	float depth = texture(samp10, float3(location + offset * resolution.zw, layer)).x;
	depth = 1 / (A * depth + B);
	return depth;	
}

float4 SampleOffset(int2 offset)
{
	return texture(samp9, float3(uv0 + offset * resolution.zw, layer));
}

float4 SamplePrevOffset(int2 offset)
{
	return SamplePrevLocationOffset(uv0, offset);
}

float SampleDepthOffset(int2 offset)
{
	return SampleDepthLocationOffset(uv0, offset);
}

float4 Sample(){ return Sample(uv0, layer); }
float4 SamplePrev(){ return SamplePrev(uv0); }
float SampleDepth() { return SampleDepth(uv0, layer); }
float4 SampleLocation(float2 location) { return Sample(location, layer); }
float4 SamplePrevLocation(float2 location) { return SamplePrev(location); }
float SampleDepthLocation(float2 location) { return SampleDepth(location, layer); }
float4 SampleLayer(int l) { return Sample(uv0, l); }
float SampleDepthLayer(int l) { return SampleDepth(uv0, l); }
float4 SampleFontLocation(float2 location) { return texture(samp8, location); }

float4 ApplyGCGamma(float4 col)
{
	return pow(col, float4(native_gamma, native_gamma, native_gamma, native_gamma));
}
float2 GetResolution()
{
	return resolution.xy;
}
float2 GetInvResolution()
{
	return resolution.zw;
}
float2 GetCoordinates()
{
	return uv0;
}
float2 GetSourceTextureSize()
{
	return textureSize(samp9, 0).xy;
}
uint GetTime()
{
	return time;
}
void SetOutput(float4 color)
{
	ocol0 = color;
}
#define mult(a, b) (a * b)
#define GetOption(x) (option_##x)
#define OptionEnabled(x) (option_##x != 0)
//Random
float global_rnd_state;
float RandomSeedfloat(float2 seed)
{
	float noise = frac(sin(dot(seed, float2(12.9898, 78.233)*2.0)) * 43758.5453);
	return noise;
}

void rnd_advance()
{
    global_rnd_state = RandomSeedfloat(uv0 + global_rnd_state);
}

uint RandomSeeduint(float2 seed)
{
	float noise = RandomSeedfloat(seed);
	return uint(noise * 0xFFFFFF);
}

void Randomize()
{
	global_rnd_state = frac(float(GetTime())*0.0001);
}

uint Rndint()
{
	rnd_advance();
	return uint(global_rnd_state * 0xFFFFFF);
}

float Rndfloat()
{
	rnd_advance();
	return global_rnd_state;
}

float2 Rndfloat2()
{
	float2 val;
	rnd_advance();
	val.x = global_rnd_state;
	rnd_advance();
	val.y = global_rnd_state;
	return val;
}

float3 Rndfloat3()
{
	float3 val;
	rnd_advance();
	val.x = global_rnd_state;
	rnd_advance();
	val.y = global_rnd_state;
	rnd_advance();
	val.z = global_rnd_state;
	return val;
}

float4 Rndfloat4()
{
	float4 val;
	rnd_advance();
	val.x = global_rnd_state;
	rnd_advance();
	val.y = global_rnd_state;
	rnd_advance();
	val.z = global_rnd_state;
	rnd_advance();
	val.w = global_rnd_state;
	return val;
}

)GLSL";
}

std::string OpenGLPostProcessing::LoadShaderOptions(const std::string& code)
{
	std::string glsl_options = "";

	for (const auto& it : m_config.GetOptions())
	{
		if (it.second.m_type == PostProcessingShaderConfiguration::ConfigurationOption::OptionType::OPTION_BOOL)
		{
			glsl_options += StringFromFormat("uniform int     option_%s;\n", it.first.c_str());
		}
		else if (it.second.m_type == PostProcessingShaderConfiguration::ConfigurationOption::OptionType::OPTION_INTEGER)
		{
			u32 count = static_cast<u32>(it.second.m_integer_values.size());
			if (count < 2)
				glsl_options += StringFromFormat("uniform int     option_%s;\n", it.first.c_str());
			else
				glsl_options += StringFromFormat("uniform int%d   option_%s;\n", count, it.first.c_str());
		}
		else if (it.second.m_type == PostProcessingShaderConfiguration::ConfigurationOption::OptionType::OPTION_FLOAT)
		{
			u32 count = static_cast<u32>(it.second.m_float_values.size());
			if (count < 2)
				glsl_options += StringFromFormat("uniform float   option_%s;\n", it.first.c_str());
			else
				glsl_options += StringFromFormat("uniform float%d option_%s;\n", count, it.first.c_str());
		}
	}

	return m_glsl_header + glsl_options + code;
}

}  // namespace OGL
