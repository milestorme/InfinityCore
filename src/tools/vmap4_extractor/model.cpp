/*
 * This file is part of the OregonCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "vmapexport.h"
#include "model.h"
#include "wmo.h"
#include "mpq_libmpq04.h"
#include <cassert>
#include <algorithm>
#include <cstdio>

Model::Model(std::string& _filename) : vertices(0), indices(0), filename(_filename)
{
}

bool Model::open(StringSet& failedPaths)
{
    MPQFile f(filename.c_str());

    ok = !f.isEof();

    if (!ok)
    {
        f.close();
        failedPaths.insert(filename);
        return false;
    }

    _unload();

    memcpy(&header, f.getBuffer(), sizeof(ModelHeader));
    if (header.nBoundingTriangles > 0)
    {
        boundingVertices = (ModelBoundingVertex*)(f.getBuffer() + header.ofsBoundingVertices);
        vertices = new Vec3D[header.nBoundingVertices];

        for (size_t i = 0; i < header.nBoundingVertices; i++)
        {
            vertices[i] = fixCoordSystem(boundingVertices[i].pos);
        }

        uint16* triangles = (uint16*)(f.getBuffer() + header.ofsBoundingTriangles);

        nIndices = header.nBoundingTriangles; // refers to the number of int16's, not the number of triangles
        indices = new uint16[nIndices];
        memcpy(indices, triangles, nIndices * 2);

        f.close();
    }
    else
    {
        //printf("not included %s\n", filename.c_str());
        f.close();
        return false;
    }
    return true;
}

bool Model::ConvertToVMAPModel(const char * outfilename)
{
    int N[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    FILE* output = fopen(outfilename, "wb");
    if (!output)
    {
        printf("Can't create the output file '%s'\n", outfilename);
        return false;
    }
    fwrite(szRawVMAPMagic,8,1,output);
    uint32 nVertices = 0;
    nVertices = header.nBoundingVertices;

    fwrite(&nVertices, sizeof(int), 1, output);
    uint32 nofgroups = 1;
    fwrite(&nofgroups, sizeof(uint32), 1, output);
    fwrite(N, 4 * 3, 1, output); // rootwmoid, flags, groupid
    fwrite(N, sizeof(float), 3 * 2, output); //bbox, only needed for WMO currently
    fwrite(N, 4, 1, output); // liquidflags
    fwrite("GRP ", 4, 1, output);
    uint32 branches = 1;
    int wsize;
    wsize = sizeof(branches) + sizeof(uint32) * branches;
    fwrite(&wsize, sizeof(int), 1, output);
    fwrite(&branches, sizeof(branches), 1, output);
    uint32 nIndexes = (uint32) nIndices;
    fwrite(&nIndexes, sizeof(uint32), 1, output);
    fwrite("INDX", 4, 1, output);
    wsize = sizeof(uint32) + sizeof(unsigned short) * nIndexes;
    fwrite(&wsize, sizeof(int), 1, output);
    fwrite(&nIndexes, sizeof(uint32), 1, output);
    if (nIndexes > 0)
    {
        for (uint32 i = 0; i < nIndices; ++i)
        {
            // index[0] -> x, index[1] -> y, index[2] -> z, index[3] -> x ...
            if ((i % 3) - 1 == 0)
            {
                uint16 tmp = indices[i];
                indices[i] = indices[i + 1];
                indices[i + 1] = tmp;
            }
        }
        fwrite(indices, sizeof(unsigned short), nIndexes, output);
    }
    fwrite("VERT", 4, 1, output);
    wsize = sizeof(int) + sizeof(float) * 3 * nVertices;
    fwrite(&wsize, sizeof(int), 1, output);
    fwrite(&nVertices, sizeof(int), 1, output);
    if (nVertices > 0)
    {
        for (uint32 vpos = 0; vpos < nVertices; ++vpos)
        {
            float tmp = vertices[vpos].y;
            vertices[vpos].y = -vertices[vpos].z;
            vertices[vpos].z = tmp;
        }
        fwrite(vertices, sizeof(float) * 3, nVertices, output);
    }

    fclose(output);

    return true;
}


Vec3D fixCoordSystem(Vec3D v)
{
    return Vec3D(v.x, v.z, -v.y);
}

Vec3D fixCoordSystem2(Vec3D v)
{
    return Vec3D(v.x, v.z, v.y);
}

ModelInstance::ModelInstance(MPQFile& f, const char* ModelInstName, uint32 mapID, uint32 tileX, uint32 tileY, FILE* pDirfile)
{
    float ff[3];
    f.read(&id, 4);
    f.read(ff, 12);
    pos = fixCoords(Vec3D(ff[0], ff[1], ff[2]));
    f.read(ff, 12);
    rot = Vec3D(ff[0], ff[1], ff[2]);

    uint16 fFlags;      // dummy var
    f.read(&scale, 2);
    f.read(&fFlags, 2); // unknown but flag 1 is used for biodome in Outland, currently this value is not used

    // scale factor - divide by 1024. blizzard devs must be on crack, why not just use a float?
    sc = scale / 1024.0f;

    char tempname[512];
    sprintf(tempname, "%s/%s", szWorkDirWmo, ModelInstName);
    FILE* input;
    input = fopen(tempname, "r+b");

    if (!input)
    {
        //printf("ModelInstance::ModelInstance couldn't open %s\n", tempname);
        return;
    }

    fseek(input, 8, SEEK_SET); // get the correct no of vertices
    int nVertices;
    fread(&nVertices, sizeof (int), 1, input);
    fclose(input);

    if (nVertices == 0)
        return;

    uint16 adtId = 0;// not used for models
    uint32 flags = MOD_M2;
    if (tileX == 65 && tileY == 65) flags |= MOD_WORLDSPAWN;
    //write mapID, tileX, tileY, Flags, ID, Pos, Rot, Scale, name
    fwrite(&mapID, sizeof(uint32), 1, pDirfile);
    fwrite(&tileX, sizeof(uint32), 1, pDirfile);
    fwrite(&tileY, sizeof(uint32), 1, pDirfile);
    fwrite(&flags, sizeof(uint32), 1, pDirfile);
    fwrite(&adtId, sizeof(uint16), 1, pDirfile);
    fwrite(&id, sizeof(uint32), 1, pDirfile);
    fwrite(&pos, sizeof(float), 3, pDirfile);
    fwrite(&rot, sizeof(float), 3, pDirfile);
    fwrite(&sc, sizeof(float), 1, pDirfile);
    uint32 nlen = strlen(ModelInstName);
    fwrite(&nlen, sizeof(uint32), 1, pDirfile);
    fwrite(ModelInstName, sizeof(char), nlen, pDirfile);
}
