#pragma once
#include <stdint.h>

//Pipe type
typedef enum{
	NODE_PIPE_IN	= 0,
	NODE_PIPE_OUT	= 1,
	NODE_PIPE_CONST	= 2
} NODE_PIPE_TYPE;

//Pipe unit
typedef enum{
	NODE_UNIT_CHAR	 = 1,
	NODE_UNIT_BOOL	 = 2,
	NODE_UNIT_INT8	 = 3,
	NODE_UNIT_INT16	 = 4,
	NODE_UNIT_INT32	 = 5,
	NODE_UNIT_INT64	 = 6,
	NODE_UNIT_UINT8	 = 7,
	NODE_UNIT_UINT16 = 8,
	NODE_UNIT_UINT32 = 9,
	NODE_UNIT_UINT64 = 10,
	NODE_UNIT_FLOAT	 = 11,
	NODE_UNIT_DOUBLE = 12
} NODE_DATA_UNIT;

//Debug mode
typedef enum{
	NODE_DEBUG_MESSAGE = 0,
	NODE_DEBUG_CSV = 1
} NODE_DEBUG_MODE;

//String of pipe type
static const char* NODE_PIPE_TYPE_STR[3] = {
	"IN",
	"OUT",
	"CONST"
};

//String of pipe unit
static const char* NODE_DATA_UNIT_STR[13] = {
	"",
	"CHAR",
	"BOOL",
	"INT8",
	"INT16",
	"INT32",
	"INT64",
	"UINT8",
	"UINT16",
	"UINT32",
	"UINT64",
	"FLOAT",
	"DOUBLE"
};

//String of debug mode
static const char* NODE_DEBUG_MODE_STR[2] = {
	"NODE_DEBUG_MESSAGE",
	"NODE_DEBUG_CSV"
};

//Size of pipe unit
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

#ifdef NODE_SYSTEM_HOST
int nodeSystemInit(uint8_t isNoLog);
int nodeSystemAddNode(char* path,char** args);
void nodeSystemPrintNodeList(int* argc,char** args);
int nodeSystemConnect(char* const inNode,char* const inPipe,char* const outNode,char* const outPipe);
int nodeSystemDisConnect(char* const inNode,char* const inPipe);
int nodeSystemSetConst(char* const constNode,char* const constPipe,int valueCount,char** setValue);
int nodeSystemSave(char* const path);
int nodeSystemLoad(char* const path);
void nodeSystemTimerRun();
void nodeSystemTimerStop();
void nodeSystemTimerSet(double period);
void nodeSystemTimerGet();
char** nodeSystemGetConst(char* const constNode,char* const constPipe,int* retCode);
char** nodeSystemGetNodeNameList(int* counts);
char** nodeSystemGetPipeNameList(char* nodeName,int* counts);
void nodeSystemExit();
#else
int nodeSystemLoop();
int nodeSystemInit();
int nodeSystemBegine();
void nodeSystemDebugLog(char* const str);
int nodeStstemSetDebugMode(NODE_DEBUG_MODE mode);
int nodeSystemRead(int pipeID,void* buffer);
int nodeSystemWrite(int pipeID,void* const buffer);
int nodeSystemAddPipe(char* const pipeName,NODE_PIPE_TYPE type,NODE_DATA_UNIT unit,uint16_t arrayLength,const void* buff);
int nodeSystemWait();
double nodeSystemGetPeriod();
#endif