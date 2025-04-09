
// macOS
#if defined(__APPLE__) && defined(__MACH__)
	#include <OpenGL/gl.h>
	#include <OpenGL/glext.h>
	
	#define glGenVertexArrays glGenVertexArraysAPPLE
	#define glBindVertexArray glBindVertexArrayAPPLE
	#define glDeleteVertexArrays glDeleteVertexArraysAPPLE

// Linux
#elif defined(__unix__)
	#include <GL/glew.h>
	
// Windows
#elif defined(WIN32)
	#include <windows.h>

	#define GL3_PROTOTYPES 1
	#include <GL/glew.h>
	#include <GL/gl.h>
#endif

#include <stb_image_write.h>

#include "system.h"
#include "render.h"
#include "mem.h"
#include "utils.h"

#include "./wipeout/game.h"


#define ATLAS_SIZE 64
#define ATLAS_GRID 32
#define ATLAS_BORDER 16

#define RENDER_TRIS_BUFFER_CAPACITY 2048
#define TEXTURES_MAX 1024


#if defined(__EMSCRIPTEN__) || defined(USE_GLES2)
	// WebGL (GLES) needs the `precision` to be set, wheras OpenGL 2 
	// doesn't like that...
	#define SHADER_SOURCE(...) "precision highp float;" #__VA_ARGS__

	// WebGL1 only allows for a 16 bit depth buffer attachment, so 
	// we sacrifice a bit of the near plane to get more precision
	// further out
	#define NEAR_PLANE 128.0
	#define FAR_PLANE (RENDER_FADEOUT_FAR)
	#define RENDER_DEPTH_BUFFER_INTERNAL_FORMAT GL_DEPTH_COMPONENT16
#else
	#define SHADER_SOURCE(...) #__VA_ARGS__

	#define NEAR_PLANE 16.0
	#define FAR_PLANE (RENDER_FADEOUT_FAR)
	#define RENDER_DEPTH_BUFFER_INTERNAL_FORMAT GL_DEPTH_COMPONENT24
#endif
	

typedef struct {
	vec2i_t offset;
	vec2i_t size;
} render_texture_t;

uint16_t RENDER_NO_TEXTURE;

#define use_program(SHADER) \
	glUseProgram((SHADER)->program); \
	glBindVertexArray((SHADER)->vao);

#define bind_va_f(index, container, member, start) \
	glVertexAttribPointer( \
		index, member_size(container, member)/sizeof(float), GL_FLOAT, false, \
		sizeof(container), \
		(GLvoid*)(offsetof(container, member) + start) \
	)

#define bind_va_color(index, container, member, start) \
	glVertexAttribPointer( \
		index, 4,  GL_UNSIGNED_BYTE, true, \
		sizeof(container), \
		(GLvoid*)(offsetof(container, member) + start) \
	)


static GLuint compile_shader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	
	GLint success;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		int log_written;
		char log[256];
		glGetShaderInfoLog(shader, 256, &log_written, log);
		die("Error compiling shader: %s\n", log);
	}
	return shader;
}

static GLuint create_program(const char *vs_source, const char *fs_source) {
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);

	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glUseProgram(program);
	return program;
}



// -----------------------------------------------------------------------------
// Main game shaders
//
static const char * const SHADER_GAME_VS = SHADER_SOURCE(
	attribute vec3 pos;
	attribute vec2 uv;
	attribute vec4 color;

	varying vec4 v_color;
	varying vec2 v_uv;
	uniform mat4 view;
	uniform mat4 model;
	uniform mat4 projection;
	uniform vec2 screen;
	uniform vec3 camera_pos;
	uniform vec2 fade;
	uniform float time;

	void main(void) {
		gl_Position = projection * view * model * vec4(pos, 1.0);
		gl_Position.xy += screen.xy * gl_Position.w;
		v_color = color;
		v_color.a *= smoothstep(
			fade.y, fade.x, // fadeout far, near
			length(vec4(camera_pos, 1.0) - model * vec4(pos, 1.0))
		);
		v_uv = uv / 2048.0; // ATLAS_GRID * ATLAS_SIZE
	}
);

static const char * const SHADER_GAME_VS_PSX = SHADER_SOURCE(
	attribute vec3 pos;
	attribute vec2 uv;
	attribute vec4 color;

	varying vec4 v_color;
	varying vec2 v_uv;
	// varying float affine;
	uniform mat4 view;
	uniform mat4 model;
	uniform mat4 projection;
	uniform vec2 screen;
	uniform vec3 camera_pos;
	uniform vec2 fade;
	uniform float time;

	//position is post MVP translation of vertex
	vec4 to_low_precision(vec4 position, vec2 resolution)
	{
		//Perform perspective divide
		vec3 perspective_divide = position.xyz / vec3(position.w);
		//Convert to screenspace coordinates
		vec2 screen_coords = (perspective_divide.xy + vec2(1.0,1.0)) * vec2(resolution.x,resolution.y) * 0.5;
		//Truncate to integer
		vec2 screen_coords_truncated = vec2(int(screen_coords.x), int(screen_coords.y));
		//Convert back to clip range -1 to 1
		vec2 reconverted_xy = ((screen_coords_truncated * vec2(2,2)) / vec2(resolution.x,resolution.y)) - vec2(1,1);
		//Construct return value
		vec4 ps1_pos = vec4(reconverted_xy.x, reconverted_xy.y, perspective_divide.z, position.w);
		ps1_pos.xyz = ps1_pos.xyz * vec3(position.w, position.w, position.w);
		return ps1_pos;
	}

	vec4 snap(vec4 vertex, vec2 resolution)
	{
		vec4 snappedPos = vertex;
		snappedPos.xyz = vertex.xyz / vertex.w; // convert to normalised device coordinates (NDC)
		snappedPos.xy = floor(resolution * snappedPos.xy) / resolution; // snap the vertex to the lower-resolution grid
		snappedPos.xyz *= vertex.w; // convert back to projection-space
		return snappedPos;
	}
	
	void main(void) {
		gl_Position = projection * view * model * vec4(pos, 1.0);
		gl_Position.xy += screen.xy * gl_Position.w;
		// gl_Position = to_low_precision(gl_Position, vec2(256, 224)); // PSX warp polygon
		// float aspect = float(screen_size.x) / float(screen_size.y); // TODO: pass screen_size
		float aspect = 16.0 / 9.0;
		vec2 screen_lowres = vec2(240.0 * aspect, 240.0);
		gl_Position = to_low_precision(gl_Position, screen_lowres); // PSX warp polygon
		// gl_Position = to_low_precision(gl_Position, vec2(320, 240)); // PSX warp polygon
		// gl_Position = snap(gl_Position, vec2(320, 240)); // PSX warp polygon
		v_color = color;
		// v_color.a *= smoothstep(
		// 	fade.y, fade.x, // fadeout far, near
		// 	length(vec4(camera_pos, 1.0) - model * vec4(pos, 1.0))
		// );
		v_uv = uv / 2048.0; // ATLAS_GRID * ATLAS_SIZE
		
		// TODO: PSX texture warp
		// vec4 vertex_mv = view * model * vec4(pos, 1.0);
		// vertex_mv.xy += screen.xy * vertex_mv.w;
		// float dist = length(vertex_mv);
		// float aff = dist + (((projection * view * model * vec4(pos, 1.0)).w * 8.0) / dist) * 0.5;
		// v_uv = v_uv * affine;
		// affine = aff;
	}
);

static const char * const SHADER_GAME_FS = SHADER_SOURCE(
	varying vec4 v_color;
	varying vec2 v_uv;
	uniform sampler2D texture;

	void main(void) {
		vec4 tex_color = texture2D(texture, v_uv);
		vec4 color = tex_color * v_color;
		if (color.a == 0.0) {
			discard;
		}
		color.rgb = color.rgb * 2.0;
		gl_FragColor = color;
	}
);

static const char * const SHADER_GAME_FS_PSX = SHADER_SOURCE(
	varying vec4 v_color;
	varying vec2 v_uv;
	uniform sampler2D texture;

	vec3 posterize(vec3 color)
	{
		float gamma = 0.6f; //6
		float num_colors = 32.0f;//32

		vec3 color_out = color;
		color_out = pow(color_out, vec3(gamma));
		color_out = color_out * num_colors;
		color_out = floor(color_out);
		color_out = color_out / num_colors;
		color_out = pow(color_out, vec3(1.0 / gamma));

		return vec3(color_out);

	}

	void main(void) {
		vec4 tex_color = texture2D(texture, v_uv);
		vec4 color = tex_color * v_color;
		if (color.a == 0.0) {
			discard;
		}
		color.rgb = posterize(color.rgb * 2.0);
		gl_FragColor = color;
	}
);

typedef struct {
	GLuint program;
	GLuint vao;
	struct {
		GLuint view;
		GLuint model;
		GLuint projection;
		GLuint screen;
		GLuint camera_pos;
		GLuint fade;
		GLuint time;
	} uniform;
	struct {
		GLuint pos;
		GLuint uv;
		GLuint color;
	} attribute;
} prg_game_t;

prg_game_t *shader_game_init(void) {
	prg_game_t *s = mem_bump(sizeof(prg_game_t));
	
	s->program = create_program(SHADER_GAME_VS, SHADER_GAME_FS);

	s->uniform.view = glGetUniformLocation(s->program, "view");
	s->uniform.model = glGetUniformLocation(s->program, "model");
	s->uniform.projection = glGetUniformLocation(s->program, "projection");
	s->uniform.screen = glGetUniformLocation(s->program, "screen");
	s->uniform.camera_pos = glGetUniformLocation(s->program, "camera_pos");
	s->uniform.fade = glGetUniformLocation(s->program, "fade");

	s->attribute.pos = glGetAttribLocation(s->program, "pos");
	s->attribute.uv = glGetAttribLocation(s->program, "uv");
	s->attribute.color = glGetAttribLocation(s->program, "color");

	glGenVertexArrays(1, &s->vao);
	glBindVertexArray(s->vao);

	glEnableVertexAttribArray(s->attribute.pos);
	glEnableVertexAttribArray(s->attribute.uv);
	glEnableVertexAttribArray(s->attribute.color);

	bind_va_f(s->attribute.pos, vertex_t, pos, 0);
	bind_va_f(s->attribute.uv, vertex_t, uv, 0);
	bind_va_color(s->attribute.color, vertex_t, color, 0);

	return s;
}

prg_game_t *shader_game_psx_init(void) {
	prg_game_t *s = mem_bump(sizeof(prg_game_t));
	
	s->program = create_program(SHADER_GAME_VS_PSX, SHADER_GAME_FS_PSX);

	s->uniform.view = glGetUniformLocation(s->program, "view");
	s->uniform.model = glGetUniformLocation(s->program, "model");
	s->uniform.projection = glGetUniformLocation(s->program, "projection");
	s->uniform.screen = glGetUniformLocation(s->program, "screen");
	s->uniform.camera_pos = glGetUniformLocation(s->program, "camera_pos");
	s->uniform.fade = glGetUniformLocation(s->program, "fade");

	s->attribute.pos = glGetAttribLocation(s->program, "pos");
	s->attribute.uv = glGetAttribLocation(s->program, "uv");
	s->attribute.color = glGetAttribLocation(s->program, "color");

	glGenVertexArrays(1, &s->vao);
	glBindVertexArray(s->vao);

	glEnableVertexAttribArray(s->attribute.pos);
	glEnableVertexAttribArray(s->attribute.uv);
	glEnableVertexAttribArray(s->attribute.color);

	bind_va_f(s->attribute.pos, vertex_t, pos, 0);
	bind_va_f(s->attribute.uv, vertex_t, uv, 0);
	bind_va_color(s->attribute.color, vertex_t, color, 0);

	return s;
}

// -----------------------------------------------------------------------------
// POST Effect shaders

static const char * const SHADER_POST_VS = SHADER_SOURCE(
	attribute vec3 pos;
	attribute vec2 uv;

	varying vec2 v_uv;

	uniform mat4 projection;
	uniform vec2 screen_size;
	uniform float time;
	
	void main(void) {
		gl_Position = projection * vec4(pos, 1.0);
		v_uv = uv;
	}
);

static const char * const SHADER_POST_FS = SHADER_SOURCE(
	varying vec2 v_uv;

	uniform sampler2D texture;
	uniform vec2 screen_size;

	void main(void) {
		gl_FragColor = texture2D(texture, v_uv);
	}
);

static const char * const SHADER_POST_FS_DITHER = SHADER_SOURCE(
	varying vec2 v_uv;

	uniform sampler2D texture;
	uniform vec2 screen_size;

	// mat8 psx_dither_big=mat8
	// (
	// 	0,48,12,60,3,51,15,63,
	// 	32,16,44,28,35,19,47,31,
	// 	8,56,4,52,11,59,7,55,
	// 	40,24,36,20,43,27,39,23,
	// 	2,50,14,62,1,49,13,61,
	// 	34,18,46,30,33,17,45,29,
	// 	10,58,6,54,9,57,5,53,
	// 	42,26,38,22,41,25,37,21
	// );

	mat4 psx_dither_table=mat4
	(
	    0,8,2,10,
	    12,4,14,6,
	    3,11,1,9,
	    15,7,13,5
	);

	vec3 dither(vec3 color, vec2 p){
	    //extrapolate 16bit color float to 16bit integer space
	    color*=255.;

	    //get dither value from dither table (by indexing it with column and row offsets)
	    int col = int(mod(p.x, 4.));
	    int row= int(mod(p.y, 4.));
	    float dither = psx_dither_table[col][row];

	    //dithering process as described in PSYDEV SDK documentation
	    color += (dither/2. - 4.);
			//
	    //clamp to 0
	    color = max(color,0.);
	    color = min(color,248.);

	    //truncate to 5bpc precision via bitwise AND operator, and limit value max to prevent wrapping.
	    //PS1 colors in default color mode have a maximum integer value of 248 (0xf8)
	    // vec3 c = vec3(color) & vec3(0xf8);
	    color = mix( vec3(color), vec3(0xf8),  step(vec3(0xf8),color) );
	    //bring color back to floating point number space
	    color/=255.; 
	    return color;
	}

	void main(void) {
	  //pixelate the texture (i.e. many pixels are mapped to the same texel/block)
	  // float downgrade = 4.0;
	  float aspect = screen_size.x / screen_size.y;
	  vec2 target_res = vec2(240.0 * aspect, 240.0);
	  // vec2 target_res = vec2(480.0 * aspect, 480);	
	  // vec2 target_res = vec2(1000, 900);
	  // vec2 target_res = screen_size/downgrade;
	  vec2 pixelated_uv = floor(v_uv * target_res)/target_res;

	  vec3 color = texture2D(texture, pixelated_uv).rgb; 

	  //scaling gl_FragCoord makes it so that the same value from dither table is used for each pixel in a block (texel)
	  color = dither(color, floor(gl_FragCoord.xy/7.0) );

	  // Send the color to the screen
	  gl_FragColor=vec4(color,1.);
	}
);

// CRT effect based on https://www.shadertoy.com/view/Ms23DR 
// by https://github.com/mattiasgustavsson/
static const char * const SHADER_POST_FS_CRT = SHADER_SOURCE(
	varying vec2 v_uv;

	uniform float time;
	uniform sampler2D texture;
	uniform vec2 screen_size;

	float FXAA_REDUCE_MIN = (1.0/ 128.0);
	float FXAA_REDUCE_MUL = (1.0 / 8.0);
	float FXAA_SPAN_MAX   = 8.0;

	//optimized version for mobile, where dependent 
	//texture reads can be a bottleneck
	vec4 fxaa(sampler2D tex, vec2 fragCoord, vec2 resolution,
		    vec2 v_rgbNW, vec2 v_rgbNE, 
		    vec2 v_rgbSW, vec2 v_rgbSE, 
		    vec2 v_rgbM) {
	    vec4 color;
	    vec2 inverseVP = vec2(1.0 / resolution.x, 1.0 / resolution.y);
	    vec3 rgbNW = texture2D(tex, v_rgbNW).xyz;
	    vec3 rgbNE = texture2D(tex, v_rgbNE).xyz;
	    vec3 rgbSW = texture2D(tex, v_rgbSW).xyz;
	    vec3 rgbSE = texture2D(tex, v_rgbSE).xyz;
	    vec4 texColor = texture2D(tex, v_rgbM);
	    vec3 rgbM  = texColor.xyz;
	    vec3 luma = vec3(0.299, 0.587, 0.114);
	    float lumaNW = dot(rgbNW, luma);
	    float lumaNE = dot(rgbNE, luma);
	    float lumaSW = dot(rgbSW, luma);
	    float lumaSE = dot(rgbSE, luma);
	    float lumaM  = dot(rgbM,  luma);
	    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
	    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
	    
	    vec2 dir;
	    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
	    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
	    
	    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
				  (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
	    
	    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	    dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX),
		      max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX),
		      dir * rcpDirMin)) * inverseVP;
	    
	    vec3 rgbA = 0.5 * (
		texture2D(tex, fragCoord * inverseVP + dir * (1.0 / 3.0 - 0.5)).xyz +
		texture2D(tex, fragCoord * inverseVP + dir * (2.0 / 3.0 - 0.5)).xyz);
	    vec3 rgbB = rgbA * 0.5 + 0.25 * (
		texture2D(tex, fragCoord * inverseVP + dir * -0.5).xyz +
		texture2D(tex, fragCoord * inverseVP + dir * 0.5).xyz);

	    float lumaB = dot(rgbB, luma);
	    if ((lumaB < lumaMin) || (lumaB > lumaMax))
		color = vec4(rgbA, texColor.a);
	    else
		color = vec4(rgbB, texColor.a);
		color = vec4(rgbB, texColor.a);
	    return color;
	}

	float random(in vec2 st) {
	    return fract(sin(dot(st.xy,
				 vec2(12.9898,78.233)))
			 * 43758.5453123);
	}

	vec2 curve(vec2 uv) {
		uv = (uv - 0.5) * 2.0;
		uv *= 1.1;	
		uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
		uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
		uv  = (uv / 2.0) + 0.5;
		uv =  uv *0.92 + 0.04;
		return uv;
	}

	void main(){
		// vec2 uv = curve(gl_FragCoord.xy / screen_size);
		vec2 uv = gl_FragCoord.xy / screen_size;
		vec3 color;
		vec3 color_clean;
		float x =  sin(0.3*time+uv.y*21.0)*sin(0.7*time+uv.y*29.0)*sin(0.3+0.33*time+uv.y*31.0)*0.0017;
		float pct = distance(uv,vec2(0.5));
		
		// vec4 fxaa(sampler2D tex, vec2 fragCoord, vec2 resolution,
		// 	    vec2 v_rgbNW, vec2 v_rgbNE, 
		// 	    vec2 v_rgbSW, vec2 v_rgbSE, 
		// 	    vec2 v_rgbM) {
		vec4 c = fxaa(texture, gl_FragCoord.xy, screen_size,
			vec2(gl_FragCoord.x-1.0, gl_FragCoord.y+1.0),
			vec2(gl_FragCoord.x+1.0, gl_FragCoord.y+1.0),
			vec2(gl_FragCoord.x-1.0, gl_FragCoord.y-1.0),
			vec2(gl_FragCoord.x+1.0, gl_FragCoord.y-1.0),
			vec2(gl_FragCoord.x, gl_FragCoord.y)
			);

		// color_clean = texture2D(texture, uv).rgb;
		color_clean = c.rgb;

		color.r = texture2D(texture, vec2(x+uv.x+0.001,uv.y+0.001)).x+0.05;
		color.g = texture2D(texture, vec2(x+uv.x+0.000,uv.y-0.002)).y+0.05;
		color.b = texture2D(texture, vec2(x+uv.x-0.002,uv.y+0.000)).z+0.05;
		color.r += 0.05*texture2D(texture, 0.75*vec2(x+0.025, -0.027)+vec2(uv.x+0.001,uv.y+0.001)).x;
		color.g += 0.03*texture2D(texture, 0.75*vec2(x+-0.022, -0.02)+vec2(uv.x+0.000,uv.y-0.002)).y;
		color.b += 0.05*texture2D(texture, 0.75*vec2(x+-0.02, -0.018)+vec2(uv.x-0.001,uv.y+0.000)).z;

		// color = clamp(color*0.6+0.4*color*color*1.0,0.0,1.0); // what?

		color = mix(color_clean, color, pow(pct, 0.5));
		// color = color_clean;
		// color.g = pow(pct, 0.5);
		// color.g = pct;

		float vignette = (0.0 + 1.0*16.0*uv.x*uv.y*(1.0-uv.x)*(1.0-uv.y));
		color *= vec3(pow(vignette, 0.30));

		// color *= vec3(0.95,1.05,0.95);
		// color *= 1.2;

		//color manipulation
		// color = pow(color, vec3(0.5));
		// color = mix(color, color*color, 0.4);
		color = mix(color, pow(color, vec3(0.5)), 0.1);
		color *= 0.7;

		// color = mix(color, smoothstep(0.0, 1.0, color), 0.1);


		// float scanlines = clamp( 0.35+0.35*sin(3.5*time+uv.y*screen_size.y*1.5), 0.0, 1.0);
		// 
		// float s = pow(scanlines,1.7);
		// color = color * vec3(0.4+0.7*s);

		//wavy?
		color *= 1.0+0.01*sin(110.0*time);
		if (uv.x < 0.0 || uv.x > 1.0)
			color *= 0.0;
		if (uv.y < 0.0 || uv.y > 1.0)
			color *= 0.0;
		
		// color*=1.0-0.65*vec3(clamp((mod(gl_FragCoord.x, 2.0)-1.0)*2.0,0.0,1.0));

		//noise
		float t = mod(time, 100.0);
		color -= 0.02*random(vec2(uv.y*t/20.12345, uv.x*t));
		color += 0.05*random(vec2(uv.x*t/5.54321, uv.y*t));

		gl_FragColor = vec4(color,1.0);
	}
);

typedef struct {
	GLuint program;
	GLuint vao;
	struct {
		GLuint projection;
		GLuint screen_size;
		GLuint time;
	} uniform;
	struct {
		GLuint pos;
		GLuint uv;
	} attribute;
} prg_post_t;

void shader_post_general_init(prg_post_t *s) {
	s->uniform.projection = glGetUniformLocation(s->program, "projection");
	s->uniform.screen_size = glGetUniformLocation(s->program, "screen_size");
	s->uniform.time = glGetUniformLocation(s->program, "time");

	s->attribute.pos = glGetAttribLocation(s->program, "pos");
	s->attribute.uv = glGetAttribLocation(s->program, "uv");

	glGenVertexArrays(1, &s->vao);
	glBindVertexArray(s->vao);

	glEnableVertexAttribArray(s->attribute.pos);
	glEnableVertexAttribArray(s->attribute.uv);

	bind_va_f(s->attribute.pos, vertex_t, pos, 0);
	bind_va_f(s->attribute.uv, vertex_t, uv, 0);
}

prg_post_t *shader_post_default_init(void) {
	prg_post_t *s = mem_bump(sizeof(prg_post_t));
	s->program = create_program(SHADER_POST_VS, SHADER_POST_FS);	
	shader_post_general_init(s);
	return s;
}

prg_post_t *shader_post_crt_init(void) {
	prg_post_t *s = mem_bump(sizeof(prg_post_t));
	s->program = create_program(SHADER_POST_VS, SHADER_POST_FS_CRT);	
	shader_post_general_init(s);
	return s;
}



// -----------------------------------------------------------------------------

static GLuint vbo;

static tris_t tris_buffer[RENDER_TRIS_BUFFER_CAPACITY];
static uint32_t tris_len = 0;

static vec2i_t screen_size;
static vec2i_t backbuffer_size;

static uint32_t atlas_map[ATLAS_SIZE] = {0};
static GLuint atlas_texture = 0;
static render_blend_mode_t blend_mode = RENDER_BLEND_NORMAL;

static mat4_t projection_mat_2d = mat4_identity();
static mat4_t projection_mat_bb = mat4_identity();
static mat4_t projection_mat_3d = mat4_identity();
static mat4_t sprite_mat = mat4_identity();
static mat4_t view_mat = mat4_identity();

static render_texture_t textures[TEXTURES_MAX];
static uint32_t textures_len = 0;
static bool texture_mipmap_is_dirty = false;

static render_resolution_t render_res;
static GLuint backbuffer = 0;
static GLuint backbuffer_texture = 0;
static GLuint backbuffer_depth_buffer = 0;

prg_game_t *prg_game;
prg_game_t *prg_game_default;
prg_game_t *prg_game_psx;
prg_post_t *prg_post;
prg_post_t *prg_post_effects[NUM_RENDER_POST_EFFCTS] = {};


static void render_flush(void);

// static void gl_message_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam) {
// 	puts(message);
// }

void render_init(vec2i_t screen_size) {	
	#if defined(__APPLE__) && defined(__MACH__)
		// OSX
		// (nothing to do here)
	#else
		// Windows, Linux
		glewExperimental = GL_TRUE;
		glewInit();
	#endif

	// glEnable(GL_DEBUG_OUTPUT);
	// glDebugMessageCallback(gl_message_callback, NULL);
	// glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);

	// Atlas Texture
	glGenTextures(1, &atlas_texture);
	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, RENDER_USE_MIPMAPS ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// float anisotropy = 64.0;
	float anisotropy = 0;
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &anisotropy);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);

	uint32_t tw = ATLAS_SIZE * ATLAS_GRID;
	uint32_t th = ATLAS_SIZE * ATLAS_GRID;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	printf("atlas texture %5d\n", atlas_texture);
	
	// Tris buffer
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);

	// Post Shaders
	prg_post_effects[RENDER_POST_NONE] = shader_post_default_init();
	prg_post_effects[RENDER_POST_CRT] = shader_post_crt_init();
	render_set_post_effect(RENDER_POST_CRT);

	// Game shader
	prg_game_default = shader_game_init();
	prg_game_psx = shader_game_psx_init();
	prg_game = (save.psx_wobble) ? prg_game_psx : prg_game_default;
	use_program(prg_game);

	render_set_view(vec3(0, 0, 0), vec3(0, 0, 0));
	render_set_model_mat(&mat4_identity());

	// Some of these hints are probably unnecessary
	glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
	glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
	glHint(GL_CLIP_VOLUME_CLIPPING_HINT_EXT, GL_NICEST);
	glHint(GL_SUBPIXEL_BITS, GL_NICEST);
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	glEnable(GL_MULTISAMPLE);
	glEnable(GL_CULL_FACE);
	// glCullFace(GL_BACK);
	glEnable(GL_BLEND);
	// glBlendFunc(GL_SRC_ALPHA_SATURATE, GL_ONE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Create white texture
	rgba_t white_pixels[4] = {
		rgba(128,128,128,255), rgba(128,128,128,255),
		rgba(128,128,128,255), rgba(128,128,128,255)
	};
	RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);

	// Backbuffer
	render_res = RENDER_RES_NATIVE;
	render_set_screen_size(screen_size);
}

void render_cleanup(void) {
	// TODO
}

static mat4_t render_setup_2d_projection_mat(vec2i_t size) {
	float near = -1;
	float far = 1;
	float left = 0;
	float right = size.x;
	float bottom = size.y;
	float top = 0;
	float lr = 1 / (left - right);
	float bt = 1 / (bottom - top);
	float nf = 1 / (near - far);
	return mat4(
		-2 * lr,  0,  0,  0,
		0,  -2 * bt,  0,  0,
		0,        0,  2 * nf,    0, 
		(left + right) * lr, (top + bottom) * bt, (far + near) * nf, 1
	);
}

static mat4_t render_setup_3d_projection_mat(vec2i_t size) {
	// wipeout has a horizontal fov of 90deg, but we want the fov to be fixed 
	// for the vertical axis, so that widescreen displays just have a wider 
	// view. For the original 4/3 aspect ratio this equates to a vertical fov
	// of 73.75deg.
	float aspect = (float)size.x / (float)size.y;
	float fov = (73.75 / 180.0) * 3.14159265358;//73.75
	float f = 1.0 / tan(fov / 2);
	float nf = 1.0 / (NEAR_PLANE - FAR_PLANE);
	return mat4(
		f / aspect, 0, 0, 0,
		0, f, 0, 0, 
		0, 0, (FAR_PLANE + NEAR_PLANE) * nf, -1, 
		0, 0, 2 * FAR_PLANE * NEAR_PLANE * nf, 0
	);
}

void render_set_projection_fov(float v_fov) {
	// wipeout has a horizontal fov of 90deg, but we want the fov to be fixed 
	// for the vertical axis, so that widescreen displays just have a wider 
	// view. For the original 4/3 aspect ratio this equates to a vertical fov
	// of 73.75deg.
	float aspect = (float)backbuffer_size.x / (float)backbuffer_size.y;
	float fov = (v_fov / 180.0) * 3.14159265358;//73.75
	float f = 1.0 / tan(fov / 2);
	float nf = 1.0 / (NEAR_PLANE - FAR_PLANE);
	projection_mat_3d = mat4(
		f / aspect, 0, 0, 0,
		0, f, 0, 0, 
		0, 0, (FAR_PLANE + NEAR_PLANE) * nf, -1, 
		0, 0, 2 * FAR_PLANE * NEAR_PLANE * nf, 0
	);
}

void render_set_screen_size(vec2i_t size) {
	screen_size = size;
	projection_mat_bb = render_setup_2d_projection_mat(screen_size);

	render_set_resolution(render_res);
}


void render_set_resolution(render_resolution_t res) {
	render_res = res;
	prg_game = (save.psx_wobble) ? prg_game_psx : prg_game_default;

	if (res == RENDER_RES_NATIVE) {
		// prg_game = prg_game_default;
		backbuffer_size = screen_size;
	}
	else {
		// prg_game = prg_game_psx;
		float aspect = (float)screen_size.x / (float)screen_size.y;
		if (res == RENDER_RES_240P) {
			backbuffer_size = vec2i(240.0 * aspect, 240);
		}
		else if (res == RENDER_RES_480P) {
			backbuffer_size = vec2i(480.0 * aspect, 480);	
		}
		else {
			die("Invalid resolution: %d", res);
		}
	}

	if (!backbuffer) {
		glGenTextures(1, &backbuffer_texture);	
		glGenFramebuffers(1, &backbuffer);
		glGenRenderbuffers(1, &backbuffer_depth_buffer);
	}
	
	glBindTexture(GL_TEXTURE_2D, backbuffer_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, backbuffer_size.x, backbuffer_size.y, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	
	glBindFramebuffer(GL_FRAMEBUFFER, backbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, backbuffer_depth_buffer);	
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, backbuffer_depth_buffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, backbuffer_texture, 0);
	
	glBindRenderbuffer(GL_RENDERBUFFER, backbuffer_depth_buffer);
	glRenderbufferStorage(GL_RENDERBUFFER, RENDER_DEPTH_BUFFER_INTERNAL_FORMAT, backbuffer_size.x, backbuffer_size.y);

	projection_mat_2d = render_setup_2d_projection_mat(backbuffer_size);
	projection_mat_3d = render_setup_3d_projection_mat(backbuffer_size);

	// Use nearest texture min filter for 240p and 480p
	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	if (res == RENDER_RES_NATIVE) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, RENDER_USE_MIPMAPS ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	}
	else {
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}
	glViewport(0, 0, backbuffer_size.x, backbuffer_size.y);
}

void render_set_post_effect(render_post_effect_t post) {
	error_if(post < 0 || post > NUM_RENDER_POST_EFFCTS, "Invalid post effect %d", post);
	prg_post = prg_post_effects[post];
}

vec2i_t render_size(void) {
	return backbuffer_size;
}

void render_frame_prepare(void) {
	use_program(prg_game);
	glBindFramebuffer(GL_FRAMEBUFFER, backbuffer);
	glViewport(0, 0, backbuffer_size.x, backbuffer_size.y);

	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glUniform2f(prg_game->uniform.screen, 0, 0);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(true);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST); 
}

void render_frame_end(void) {
	render_flush();

	use_program(prg_post);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, screen_size.x, screen_size.y);
	glBindTexture(GL_TEXTURE_2D, backbuffer_texture);
	glUniformMatrix4fv(prg_post->uniform.projection, 1, false, projection_mat_bb.m);
	glUniform1f(prg_post->uniform.time, system_cycle_time());
	glUniform2f(prg_post->uniform.screen_size, screen_size.x, screen_size.y);

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	rgba_t white = rgba(128,128,128,255);
	tris_buffer[tris_len++] = (tris_t){
		.vertices = {
			{.pos = {0, screen_size.y, 0}, .uv = {0, 0}, .color = white},
			{.pos = {screen_size.x, 0, 0}, .uv = {1, 1}, .color = white},
			{.pos = {0, 0, 0}, .uv = {0, 1}, .color = white},
		}
	};
	tris_buffer[tris_len++] = (tris_t){
		.vertices = {
			{.pos = {screen_size.x, screen_size.y, 0}, .uv = {1, 0}, .color = white},
			{.pos = {screen_size.x, 0, 0}, .uv = {1, 1}, .color = white},
			{.pos = {0, screen_size.y, 0}, .uv = {0, 0}, .color = white},
		}
	};

	render_flush();
}

void render_flush(void) {
	if (tris_len == 0) {
		return;
	}

	if (texture_mipmap_is_dirty) {
		glGenerateMipmap(GL_TEXTURE_2D);
		texture_mipmap_is_dirty = false;
	}

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tris_t) * tris_len, tris_buffer, GL_DYNAMIC_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, tris_len * 3);
	tris_len = 0;
}

void render_set_view(vec3_t pos, vec3_t angles) {
	render_flush();
	render_set_depth_write(true);
	render_set_depth_test(true);

	view_mat = mat4_identity();
	mat4_set_translation(&view_mat, vec3(0, 0, 0));
	mat4_set_roll_pitch_yaw(&view_mat, vec3(angles.x, -angles.y + M_PI, angles.z + M_PI));
	mat4_translate(&view_mat, vec3_inv(pos));
	mat4_set_yaw_pitch_roll(&sprite_mat, vec3(-angles.x, angles.y - M_PI, 0));

	render_set_model_mat(&mat4_identity());

	render_flush();
	glUniformMatrix4fv(prg_game->uniform.view, 1, false, view_mat.m);
	glUniformMatrix4fv(prg_game->uniform.projection, 1, false, projection_mat_3d.m);
	glUniform3f(prg_game->uniform.camera_pos, pos.x, pos.y, pos.z);
	glUniform2f(prg_game->uniform.fade, RENDER_FADEOUT_NEAR, RENDER_FADEOUT_FAR);
}

void render_set_view_2d(void) {
	render_flush();
	render_set_depth_test(false);
	render_set_depth_write(false);

	render_set_model_mat(&mat4_identity());
	glUniform3f(prg_game->uniform.camera_pos, 0, 0, 0);
	glUniformMatrix4fv(prg_game->uniform.view, 1, false, mat4_identity().m);
	glUniformMatrix4fv(prg_game->uniform.projection, 1, false, projection_mat_2d.m);
}

void render_set_model_mat(mat4_t *m) {
	render_flush();
	glUniformMatrix4fv(prg_game->uniform.model, 1, false, m->m);
}

void render_set_depth_write(bool enabled) {
	render_flush();
	glDepthMask(enabled);
}

void render_set_depth_test(bool enabled) {
	render_flush();
	if (enabled) {
		glEnable(GL_DEPTH_TEST);
	}
	else {
		glDisable(GL_DEPTH_TEST); 
	}
}

void render_set_depth_offset(float offset) {
	render_flush();
	if (offset == 0) {
		glDisable(GL_POLYGON_OFFSET_FILL);
		return;	
	}

	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(offset, 1.0);
}

void render_set_screen_position(vec2_t pos) {
	render_flush();
	glUniform2f(prg_game->uniform.screen, pos.x, -pos.y);
}

void render_set_blend_mode(render_blend_mode_t new_mode) {
	if (new_mode == blend_mode) {
		return;
	}
	render_flush();

	blend_mode = new_mode;
	if (blend_mode == RENDER_BLEND_NORMAL) {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else if (blend_mode == RENDER_BLEND_LIGHTER) {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		// glBlendFunc(GL_ONE, GL_ONE);
	}
}

void render_set_cull_backface(bool enabled) {
	render_flush();
	if (enabled) {
		glEnable(GL_CULL_FACE);
	}
	else {
		glDisable(GL_CULL_FACE);
	}
}

vec3_t render_transform(vec3_t pos) {
	return vec3_transform(vec3_transform(pos, &view_mat), &projection_mat_3d);
}

void render_push_tris(tris_t tris, uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	
	if (tris_len >= RENDER_TRIS_BUFFER_CAPACITY) {
		render_flush();
	}

	render_texture_t *t = &textures[texture_index];

	for (int i = 0; i < 3; i++) {
		tris.vertices[i].uv.x += t->offset.x;
		tris.vertices[i].uv.y += t->offset.y;
	}
	tris_buffer[tris_len++] = tris;
}

void render_push_sprite(vec3_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);

	vec3_t p1 = vec3_add(pos, vec3_transform(vec3(-size.x * 0.5, -size.y * 0.5, 0), &sprite_mat));
	vec3_t p2 = vec3_add(pos, vec3_transform(vec3( size.x * 0.5, -size.y * 0.5, 0), &sprite_mat));
	vec3_t p3 = vec3_add(pos, vec3_transform(vec3(-size.x * 0.5,  size.y * 0.5, 0), &sprite_mat));
	vec3_t p4 = vec3_add(pos, vec3_transform(vec3( size.x * 0.5,  size.y * 0.5, 0), &sprite_mat));

	render_texture_t *t = &textures[texture_index];
	render_push_tris((tris_t){
		.vertices = {
			{
				.pos = p1,
				.uv = {0, 0},
				.color = color
			},
			{
				.pos = p2,
				.uv = {0 + t->size.x ,0},
				.color = color
			},
			{
				.pos = p3,
				.uv = {0, 0 + t->size.y},
				.color = color
			},
		}
	}, texture_index);
	render_push_tris((tris_t){
		.vertices = {
			{
				.pos = p3,
				.uv = {0, 0 + t->size.y},
				.color = color
			},
			{
				.pos = p2,
				.uv = {0 + t->size.x, 0},
				.color = color
			},
			{
				.pos = p4,
				.uv = {0 + t->size.x, 0 + t->size.y},
				.color = color
			},
		}
	}, texture_index);
}

void render_push_2d(vec2i_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
	render_push_2d_tile(pos, vec2i(0, 0), render_texture_size(texture_index), size, color, texture_index);
}

void render_push_2d_tile(vec2i_t pos, vec2i_t uv_offset, vec2i_t uv_size, vec2i_t size, rgba_t color, uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	render_push_tris((tris_t){
		.vertices = {
			{
				.pos = {pos.x, pos.y + size.y, 0},
				.uv = {uv_offset.x , uv_offset.y + uv_size.y},
				.color = color
			},
			{
				.pos = {pos.x + size.x, pos.y, 0},
				.uv = {uv_offset.x +  uv_size.x, uv_offset.y},
				.color = color
			},
			{
				.pos = {pos.x, pos.y, 0},
				.uv = {uv_offset.x , uv_offset.y},
				.color = color
			},
		}
	}, texture_index);

	render_push_tris((tris_t){
		.vertices = {
			{
				.pos = {pos.x + size.x, pos.y + size.y, 0},
				.uv = {uv_offset.x + uv_size.x, uv_offset.y + uv_size.y},
				.color = color
			},
			{
				.pos = {pos.x + size.x, pos.y, 0},
				.uv = {uv_offset.x + uv_size.x, uv_offset.y},
				.color = color
			},
			{
				.pos = {pos.x, pos.y + size.y, 0},
				.uv = {uv_offset.x , uv_offset.y + uv_size.y},
				.color = color
			},
		}
	}, texture_index);
}

uint16_t render_texture_create(uint32_t tw, uint32_t th, rgba_t *pixels) {
	error_if(textures_len >= TEXTURES_MAX, "TEXTURES_MAX reached");

	uint32_t bw = tw + ATLAS_BORDER * 2;
	uint32_t bh = th + ATLAS_BORDER * 2;

	// Find a position in the atlas for this texture (with added border)
	uint32_t grid_width = (bw + ATLAS_GRID - 1) / ATLAS_GRID;
	uint32_t grid_height = (bh + ATLAS_GRID - 1) / ATLAS_GRID;
	uint32_t grid_x = 0;
	uint32_t grid_y = ATLAS_SIZE - grid_height + 1;

	for (uint32_t cx = 0; cx < ATLAS_SIZE - grid_width; cx++) {
		if (atlas_map[cx] >= grid_y) {
			continue;
		}

		uint32_t cy = atlas_map[cx];
		bool is_best = true;

		for (uint32_t bx = cx; bx < cx + grid_width; bx++) {
			if (atlas_map[bx] >= grid_y) {
				is_best = false;
				cx = bx;
				break;
			}
			if (atlas_map[bx] > cy) {
				cy = atlas_map[bx];
			}
		}
		if (is_best) {
			grid_y = cy;
			grid_x = cx;
		}
	}

	error_if(grid_y + grid_height > ATLAS_SIZE, "Render atlas ran out of space");

	for (uint32_t cx = grid_x; cx < grid_x + grid_width; cx++) {
		atlas_map[cx] = grid_y + grid_height;
	}

	// Add the border pixels for this texture
	rgba_t *pb = mem_temp_alloc(sizeof(rgba_t) * bw * bh);

	if (tw && th) {
		// Top border
		for (int32_t y = 0; y < ATLAS_BORDER; y++) {
			memcpy(pb + bw * y + ATLAS_BORDER, pixels, tw * sizeof(rgba_t));
		}

		// Bottom border
		for (int32_t y = 0; y < ATLAS_BORDER; y++) {
			memcpy(pb + bw * (bh - ATLAS_BORDER + y) + ATLAS_BORDER, pixels + tw * (th-1), tw * sizeof(rgba_t));
		}
		
		// Left border
		for (int32_t y = 0; y < bh; y++) {
			for (int32_t x = 0; x < ATLAS_BORDER; x++) {
				pb[y * bw + x] = pixels[clamp(y-ATLAS_BORDER, 0, th-1) * tw];
			}
		}

		// Right border
		for (int32_t y = 0; y < bh; y++) {
			for (int32_t x = 0; x < ATLAS_BORDER; x++) {
				pb[y * bw + x + bw - ATLAS_BORDER] = pixels[tw - 1 + clamp(y-ATLAS_BORDER, 0, th-1) * tw];
			}
		}

		// Texture
		for (int32_t y = 0; y < th; y++) {
			memcpy(pb + bw * (y + ATLAS_BORDER) + ATLAS_BORDER, pixels + tw * y, tw * sizeof(rgba_t));
		}
	}

	uint32_t x = grid_x * ATLAS_GRID;
	uint32_t y = grid_y * ATLAS_GRID;
	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, pb);
	mem_temp_free(pb);


	texture_mipmap_is_dirty = RENDER_USE_MIPMAPS;
	uint16_t texture_index = textures_len;
	textures_len++;
	textures[texture_index] = (render_texture_t){ {x + ATLAS_BORDER, y + ATLAS_BORDER}, {tw, th} };

	// printf("inserted atlas texture (%3dx%3d) at (%3d,%3d)\n", tw, th, grid_x, grid_y);
	return texture_index;
}

vec2i_t render_texture_size(uint16_t texture_index) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
	return textures[texture_index].size;
}

void render_texture_replace_pixels(int16_t texture_index, rgba_t *pixels) {
	error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);

	render_texture_t *t = &textures[texture_index];
	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, t->offset.x, t->offset.y, t->size.x, t->size.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

uint16_t render_textures_len(void) {
	return textures_len;
}

void render_textures_reset(uint16_t len) {
	error_if(len > textures_len, "Invalid texture reset len %d >= %d", len, textures_len);
	render_flush();

	textures_len = len;
	clear(atlas_map);

	// Clear completely and recreate the default white texture
	if (len == 0) {
		rgba_t white_pixels[4] = {
			rgba(128,128,128,255), rgba(128,128,128,255),
			rgba(128,128,128,255), rgba(128,128,128,255)
		};
		RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);
		return;
	}

	// Replay all texture grid insertions up to the reset len
	for (int i = 0; i < textures_len; i++) {
		uint32_t grid_x = (textures[i].offset.x - ATLAS_BORDER) / ATLAS_GRID;
		uint32_t grid_y = (textures[i].offset.y - ATLAS_BORDER) / ATLAS_GRID;
		uint32_t grid_width = (textures[i].size.x + ATLAS_BORDER * 2 + ATLAS_GRID - 1) / ATLAS_GRID;
		uint32_t grid_height = (textures[i].size.y + ATLAS_BORDER * 2 + ATLAS_GRID - 1) / ATLAS_GRID;
		for (uint32_t cx = grid_x; cx < grid_x + grid_width; cx++) {
			atlas_map[cx] = grid_y + grid_height;
		}
	}
}

void render_textures_dump(const char *path) {
	int width = ATLAS_SIZE * ATLAS_GRID;
	int height = ATLAS_SIZE * ATLAS_GRID;
	rgba_t *pixels = malloc(sizeof(rgba_t) * width * height);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	stbi_write_png(path, width, height, 4, pixels, 0);
	free(pixels);
}
