#pragma once
#include <stdint.h>

typedef enum{
	NODE_IN		= 0,
	NODE_OUT	= 1,
	NODE_CONST	= 2
} NODE_PIPE_TYPE;

typedef enum{
	NODE_CHAR		= 1,
	NODE_BOOL		= 2,
	NODE_INT_8		= 3,
	NODE_INT_16		= 4,
	NODE_INT_32		= 5,
	NODE_INT_64		= 6,
	NODE_UINT_8		= 7,
	NODE_UINT_16	= 8,
	NODE_UINT_32	= 9,
	NODE_UINT_64	= 10,
	NODE_FLOAT		= 11,
	NODE_DOUBLE		= 12
} NODE_DATA_UNIT;

static const char* NODE_DATA_UNIT_STR[13] = {
	"",
	"CHAR",
	"BOOL",
	"INT_8",
	"INT_16",
	"INT_32",
	"INT_64",
	"UINT_8",
	"UINT_16",
	"UINT_32",
	"UINT_64",
	"FLOAT",
	"DOUBLE"
};

static const uint16_t NODE_DATA_UNIT_SIZE[13] = {
	0,
	sizeof(char),
	1,
	sizeof(int8_t),
	sizeof(int16_t),
	sizeof(int32_t),
	sizeof(int64_t),
	sizeof(uint8_t),
	sizeof(uint16_t),
	sizeof(uint32_t),
	sizeof(uint64_t),
	sizeof(float),
	sizeof(double)
};

int nodeSystemInit();
int nodeSystemAddPipe(char* const pipeName,NODE_PIPE_TYPE type,NODE_DATA_UNIT unit,uint16_t arrayLength);
int nodeSystemBegine();
int nodeSystemLoop();
void nodeSystemDebugLog(char* const str);
int nodeSystemRead(int pipeID,void* buffer,uint16_t size);
int nodeSystemWrite(int pipeID,void* buffer,uint16_t size);
