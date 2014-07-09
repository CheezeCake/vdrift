/************************************************************************/
/*                                                                      */
/* This file is part of VDrift.                                         */
/*                                                                      */
/* VDrift is free software: you can redistribute it and/or modify       */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* VDrift is distributed in the hope that it will be useful,            */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU General Public License for more details.                         */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */
/* along with VDrift.  If not, see <http://www.gnu.org/licenses/>.      */
/*                                                                      */
/************************************************************************/

#include "model_joe03.h"
#include "joepack.h"
#include "mathvector.h"
#include "endian_utility.h"
#include "unordered_map.h"

#include <vector>
using std::vector;

const unsigned int ModelJoe03::JOE_MAX_FACES = 32000;
const unsigned int ModelJoe03::JOE_VERSION = 3;
const float ModelJoe03::MODEL_SCALE = 1.0;

// 3 verts per face results in JOE_MAX_FACES * 3 verts limit
const unsigned int JOE_MAX_VERTS = ModelJoe03::JOE_MAX_FACES * 3;

// This holds the header information that is read in at the beginning of the file
struct JoeHeader
{
	unsigned int magic;                   // This is used to identify the file
	unsigned int version;                 // The version number of the file
	unsigned int num_faces;	            // The number of faces (polygons)
	unsigned int num_frames;               // The number of animation frames
};

// This is used to store the vertices that are read in for the current frame
struct JoeVertex
{
	float vertex[3];
};

// This stores the indices into the vertex and texture coordinate arrays
struct JoeFace
{
	unsigned short vertexIndex[3];
	unsigned short normalIndex[3];
	unsigned short textureIndex[3];
};

// This stores UV coordinates
struct JoeTexCoord
{
	float u, v;
};

// This stores the frames vertices after they have been transformed
struct JoeFrame
{
	unsigned int num_verts;
	unsigned int num_texcoords;
	unsigned int num_normals;

	std::vector<JoeFace> faces;
	std::vector<JoeVertex> verts;
	std::vector<JoeVertex> normals;
	std::vector<JoeTexCoord> texcoords;
};

// This holds all the information for our model/scene.
struct JoeObject
{
	JoeHeader info;
	std::vector<JoeFrame> frames;
};

// Unique vertex entry
struct Vert
{
	unsigned short vi;
	unsigned short ti;
	unsigned short ni;

	Vert(unsigned short nvi, unsigned short nti, unsigned short nni) :
		vi(nvi), ti(nti), ni(nni)
	{
		// ctor
	}

	bool operator ==(const Vert & v) const
	{
		return (vi == v.vi) && (ti == v.ti) && (ni == v.ni);
	}
};

struct VertHash
{
	size_t operator ()(const Vert & v) const
	{
		const size_t h1 = (size_t(v.vi) << 16) | size_t(v.ti);
		const size_t h2 = (size_t(v.ti) << 16) | size_t(v.ni);
		return std::tr1::hash<size_t>()(h1) ^ std::tr1::hash<size_t>()(h2);
	}
};

static void CorrectEndian(std::vector<JoeFace> & p)
{
	for (unsigned int i = 0; i < p.size(); ++i)
	{
		for (unsigned int d = 0; d < 3; ++d)
		{
			p[i].vertexIndex[d] = ENDIAN_SWAP_16 ( p[i].vertexIndex[d] );
			p[i].normalIndex[d] = ENDIAN_SWAP_16 ( p[i].normalIndex[d] );
			p[i].textureIndex[d] = ENDIAN_SWAP_16 ( p[i].textureIndex[d] );
		}
	}
}

static void CorrectEndian(std::vector<JoeVertex> & p)
{
	for (unsigned int i = 0; i < p.size(); ++i)
	{
		for (unsigned int d = 0; d < 3; ++d)
		{
			p[i].vertex[d] = ENDIAN_SWAP_FLOAT ( p[i].vertex[d] );
		}
	}
}

static void CorrectEndian(std::vector<JoeTexCoord> & p)
{
	for (unsigned int i = 0; i < p.size(); ++i)
	{
		p[i].u = ENDIAN_SWAP_FLOAT ( p[i].u );
		p[i].v = ENDIAN_SWAP_FLOAT ( p[i].v );
	}
}

static int BinaryRead ( void * buffer, unsigned int size, unsigned int count, FILE * f, const JoePack * pack )
{
	unsigned int bytesread = 0;

	if ( pack == NULL )
	{
		bytesread = fread ( buffer, size, count, f );
	}
	else
	{
		bytesread = pack->fread ( buffer, size, count );
	}

	assert(bytesread == count);

	return bytesread;
}

///fix invalid normals (my own fault, i suspect.  the DOF converter i wrote may have flipped Y & Z normals)
static bool NeedsNormalSwap(JoeObject & object)
{
	bool need_normal_flip = false;
	for (unsigned int f = 0; f < object.info.num_frames; f++)
	{
		unsigned int normal_flip_count = 0;
		for (unsigned int i = 0; i < object.info.num_faces; i++)
		{
			Vec3 tri[3];
			Vec3 norms[3];
			for (unsigned int v = 0; v < 3; v++)
			{
				assert(object.frames[f].faces[i].vertexIndex[v] < object.frames[f].num_verts);
				assert(object.frames[f].faces[i].normalIndex[v] < object.frames[f].num_normals);
				tri[v].Set(object.frames[f].verts[object.frames[f].faces[i].vertexIndex[v]].vertex);
				norms[v].Set(object.frames[f].normals[object.frames[f].faces[i].normalIndex[v]].vertex);
			}
			Vec3 norm;
			for (unsigned int v = 0; v < 3; v++)
				norm = norm + norms[v];
			Vec3 tnorm = (tri[2] - tri[0]).cross(tri[1] - tri[0]);
			if (tnorm.Magnitude() > 0.0001 && norm.Magnitude() > 0.0001)
			{
				norm = norm.Normalize();
				tnorm = tnorm.Normalize();
				if (norm.dot(tnorm) < 0.5 && norm.dot(tnorm) > -0.5)
				{
					normal_flip_count++;
					//std::cout << norm.dot(tnorm) << std::endl;
					//std::cout << norm << " -- " << tnorm << std::endl;
				}
			}
		}

		if (normal_flip_count > object.info.num_faces / 4)
			need_normal_flip = true;
	}
	return need_normal_flip;
}

bool ModelJoe03::Load ( const std::string & filename, std::ostream & err_output, bool genlist, const JoePack * pack)
{
	Clear();

	FILE * m_FilePointer = NULL;

	//open file
	if ( pack == NULL )
	{
		m_FilePointer = fopen(filename.c_str(), "rb");
		if (!m_FilePointer)
		{
			err_output << "MODEL_JOE03: Failed to open file " << filename << std::endl;
			return false;
		}
	}
	else
	{
		if (!pack->fopen(filename))
		{
			err_output << "MODEL_JOE03: Failed to open file " << filename << " in " << pack->GetPath() << std::endl;
			return false;
		}
	}

	bool val = LoadFromHandle ( m_FilePointer, pack, err_output );

	// Clean up after everything
	if ( pack == NULL )
		fclose ( m_FilePointer );
	else
		pack->fclose();

	if (val)
	{
		if (!genlist)
		{
			//optimize into vertex array/buffers
			GenVertexArrayObject(err_output);
		}
	}
	else
	{
		err_output << "in " << filename << std::endl;
	}

	return val;
}

bool ModelJoe03::LoadFromHandle ( FILE * m_FilePointer, const JoePack * pack, std::ostream & err_output )
{
	JoeObject object;

	// Read the header data and store it in our variable
	BinaryRead ( &object.info, sizeof ( JoeHeader ), 1, m_FilePointer, pack );

	object.info.magic = ENDIAN_SWAP_32 ( object.info.magic );
	object.info.version = ENDIAN_SWAP_32 ( object.info.version );
	object.info.num_faces = ENDIAN_SWAP_32 ( object.info.num_faces );
	object.info.num_frames = ENDIAN_SWAP_32 ( object.info.num_frames );

	// Make sure the version is what we expect or else it's a bad egg
	if ( object.info.version != JOE_VERSION )
	{
		// Display an error message for bad file format, then stop loading
		err_output << "Invalid file format (Version is " << object.info.version << " not " << JOE_VERSION << "). ";
		return false;
	}

	if ( object.info.num_faces > JOE_MAX_FACES )
	{
		err_output << object.info.num_faces << " faces (max " << JOE_MAX_FACES << "). ";
		return false;
	}

	// Read in the model data
	ReadData ( m_FilePointer, pack, object );

	//generate metrics such as bounding box, etc
	GenMeshMetrics();

	// Return a success
	return true;
}

void ModelJoe03::ReadData ( FILE * m_FilePointer, const JoePack * pack, JoeObject & object )
{
	unsigned int num_frames = object.info.num_frames;
	unsigned int num_faces = object.info.num_faces;

	object.frames.resize(num_frames);

	for ( unsigned int i = 0; i < num_frames; i++ )
	{
		object.frames[i].faces.resize(num_faces);

		BinaryRead ( &object.frames[i].faces[0], sizeof ( JoeFace ), num_faces, m_FilePointer, pack );
		CorrectEndian ( object.frames[i].faces );

		BinaryRead ( &object.frames[i].num_verts, sizeof ( unsigned int ), 1, m_FilePointer, pack );
		object.frames[i].num_verts = ENDIAN_SWAP_32 ( object.frames[i].num_verts );
		BinaryRead ( &object.frames[i].num_texcoords, sizeof ( unsigned int ), 1, m_FilePointer, pack );
		object.frames[i].num_texcoords = ENDIAN_SWAP_32 ( object.frames[i].num_texcoords );
		BinaryRead ( &object.frames[i].num_normals, sizeof ( unsigned int ), 1, m_FilePointer, pack );
		object.frames[i].num_normals = ENDIAN_SWAP_32 ( object.frames[i].num_normals );

		object.frames[i].verts.resize(object.frames[i].num_verts);
		object.frames[i].normals.resize(object.frames[i].num_normals);
		object.frames[i].texcoords.resize(object.frames[i].num_texcoords);

		BinaryRead ( &object.frames[i].verts[0], sizeof ( JoeVertex ), object.frames[i].num_verts, m_FilePointer, pack );
		CorrectEndian ( object.frames[i].verts );
		BinaryRead ( &object.frames[i].normals[0], sizeof ( JoeVertex ), object.frames[i].num_normals, m_FilePointer, pack );
		CorrectEndian ( object.frames[i].normals );
		BinaryRead ( &object.frames[i].texcoords[0], sizeof ( JoeTexCoord ), object.frames[i].num_texcoords, m_FilePointer, pack );
		CorrectEndian ( object.frames[i].texcoords );
	}

	//cout << "!!! loading " << modelpath << endl;

	//go do scaling
	for (unsigned int i = 0; i < num_frames; i++)
	{
		for ( unsigned int v = 0; v < object.frames[i].num_verts; v++ )
		{
			Vec3 temp;

			temp.Set ( object.frames[i].verts[v].vertex );
			temp = temp * MODEL_SCALE;

			for (int n = 0; n < 3; n++)
				object.frames[i].verts[v].vertex[n] = temp[n];
		}
	}

	if (NeedsNormalSwap(object))
	{
		for (unsigned int i = 0; i < num_frames; i++)
		{
			for ( unsigned int v = 0; v < object.frames[i].num_normals; v++ )
			{
				std::swap(object.frames[i].normals[v].vertex[1],
					  object.frames[i].normals[v].vertex[2]);
				object.frames[i].normals[v].vertex[1] = -object.frames[i].normals[v].vertex[1];
			}
		}
		//std::cout << "!!! swapped normals !!!" << std::endl;
	}

	//assert(!NeedsNormalFlip(object));

	/*//make sure vertex ordering is consistent with normals
	for (unsigned int i = 0; i < object.info.num_faces; i++)
	{
		unsigned short vi[3];
		VERTEX tri[3];
		VERTEX norms[3];
		for (unsigned int v = 0; v < 3; v++)
		{
			vi[v] = GetFace(i)[v];
			tri[v].Set(GetVert(vi[v]));
			norms[v].Set(GetNorm(GetNormIdx(i)[v]));
		}
		VERTEX norm;
		for (unsigned int v = 0; v < 3; v++)
			norm = norm + norms[v];
		norm = norm.normalize();
		VERTEX tnorm = (tri[2] - tri[0]).cross(tri[1] - tri[0]);
		if (norm.dot(tnorm) > 0)
		{
			short tvi = object.frames[0].faces[i].vertexIndex[1];
			object.frames[0].faces[i].vertexIndex[1] = object.frames[0].faces[i].vertexIndex[2];
			object.frames[0].faces[i].vertexIndex[2] = tvi;

			tvi = object.frames[0].faces[i].normalIndex[1];
			object.frames[0].faces[i].normalIndex[1] = object.frames[0].faces[i].normalIndex[2];
			object.frames[0].faces[i].normalIndex[2] = tvi;

			tvi = object.frames[0].faces[i].textureIndex[1];
			object.frames[0].faces[i].textureIndex[1] = object.frames[0].faces[i].textureIndex[2];
			object.frames[0].faces[i].textureIndex[2] = tvi;
		}
	}*/

	//build unique vertices
	//cout << "building unique vertices...." << endl;

	typedef std::tr1::unordered_map<Vert, unsigned int, VertHash> VertMap;
	VertMap vmap(object.info.num_faces * 3);

	vector <unsigned int> v_faces(object.info.num_faces * 3);

	unsigned int vnum = 0;
	for (unsigned int i = 0; i < object.info.num_faces; i++)
	{
		const JoeFace & f = object.frames[0].faces[i];
		for (unsigned int j = 0; j < 3; j++)
		{
			const Vert vert(f.vertexIndex[j], f.textureIndex[j], f.normalIndex[j]);
			std::pair<VertMap::iterator, bool> r = vmap.insert(std::make_pair(vert, vnum));
			if (r.second)
				vnum++;

			v_faces[i * 3 + j] = r.first->second;
		}
	}

	vector <float> v_vertices(vnum * 3);
	vector <float> v_texcoords(vnum * 2);
	vector <float> v_normals(vnum * 3);
	for (VertMap::const_iterator i = vmap.begin(); i != vmap.end(); i++)
	{
		const Vert & v = i->first;
		const unsigned int vi = i->second;

		for (unsigned int j = 0; j < 3; j++)
			v_vertices[vi * 3 + j] = object.frames[0].verts[v.vi].vertex[j];

		for (unsigned int j = 0; j < 3; j++)
			v_normals[vi * 3 + j] = object.frames[0].normals[v.ni].vertex[j];

		v_texcoords[vi * 2 + 0] = object.frames[0].texcoords[v.ti].u;
		v_texcoords[vi * 2 + 1] = object.frames[0].texcoords[v.ti].v;
	}

	//assign to our mesh
	m_mesh.Add(
		&v_faces[0], v_faces.size(),
		&v_vertices[0], v_vertices.size(),
		&v_texcoords[0], v_texcoords.size(),
		&v_normals[0], v_normals.size());
}

