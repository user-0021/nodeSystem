#include "nodeSystem.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <linux/limits.h>
#ifdef NODE_SYSTEM_HOST
#include <linear_list.h>
#endif

//check define macro
#define CHECK(left,right) if((left) != (right))return -1

//share memory struct
typedef struct
{
	int shmId;
	int semId;
	void* shmMap;
}shm_key;

//system global value
typedef struct{
	uint8_t isNoLog;
	time_t timeOffset;
	long period;
} nodeSystemEnv;


//local lib func
static char* getRealTimeStr();
static int debugPrintf(const char* fmt,...);
static int fileRead(   int fd,void* buf,ssize_t size);
static int fileReadStr(int fd,char* str,ssize_t size);
static int fileReadWithTimeOut(   int fd,void* buf,ssize_t size,uint32_t usec);
static int fileReadStrWithTimeOut(int fd,char* str,ssize_t size,uint32_t usec);
static int fileWrite(   int fd,const void* buf,ssize_t size);
static int fileWriteStr(int fd,const char* str);
static int shareMemoryGenerate(size_t size,shm_key* shm);
static int shareMemoryDeleate(shm_key* shm);
static int shareMemoryOpen(shm_key* shm,int shmFlag);
static int shareMemoryClose(shm_key* shm);
static int shareMemoryRead(shm_key* shm,void* buf,size_t size);
static int shareMemoryWrite(shm_key* shm,void* buf,size_t size);
static int shareMemoryLock(shm_key* shm);
static int shareMemoryUnLock(shm_key* shm);

//global
static FILE* logFile;
static shm_key systemSettingKey;
static nodeSystemEnv* systemSettingMemory = NULL;

//適当マジックナンバー　破滅的な変更のたびに変えて行く
static const uint32_t _node_init_head = 0x83DFC690;
static const uint32_t _node_init_eof  = 0x85CBADEF;
static const uint32_t _node_begin_head = 0x9067F3A2;
static const uint32_t _node_begin_eof  = 0x910AC8BB;

#ifdef NODE_SYSTEM_HOST

//state enum
enum _pipeHead{
	PIPE_ADD_NODE = 0,
	PIPE_NODE_LIST = 1,
	PIPE_NODE_CONNECT = 2,
	PIPE_NODE_DISCONNECT = 3,
	PIPE_NODE_SET_CONST = 4,
	PIPE_NODE_GET_CONST = 5,
	PIPE_GET_NODE_NAME_LIST = 6,
	PIPE_GET_PIPE_NAME_LIST = 7,
	PIPE_SAVE = 8,
	PIPE_LOAD = 9,
	PIPE_TIMER_RUN = 10,
	PIPE_TIMER_STOP = 11,
	PIPE_TIMER_SET = 12,
	PIPE_TIMER_GET = 13
};

typedef struct{
	char* pipeName;
	uint16_t length;
	NODE_PIPE_TYPE type;
	NODE_DATA_UNIT unit;
	char* connectNode;
	char* connectPipe;
	shm_key shm;
}nodePipe;

typedef struct{
	int pid;
	int fd[3];
	char* name;
	char* filePath;
	uint16_t pipeCount;
	nodePipe* pipes;
}nodeData;

typedef struct{
	enum _pipeHead op;
	void (*func)();
}node_op;

//local func
static void nodeSystemLoop();
static int nodeBegin(nodeData* node);
static void nodeDeleate(nodeData* node);
static int receiveNodeProperties(nodeData* node);
static int popenRWasNonBlock(const char const * command,int* fd);

static void pipeAddNode();
static void pipeNodeList();
static void pipeNodeConnect();
static void pipeNodeDisConnect();
static void pipeNodeSetConst();
static void pipeNodeGetConst();
static void pipeSave();
static void pipeLoad();
static void pipeTimerRun();
static void pipeTimerStop();
static void pipeTimerSet();
static void pipeTimerGet();
static void pipeGetNodeNameList();
static void pipeGetPipeNameList();

//op list
static const node_op opTable[] = {
	{.op=PIPE_ADD_NODE			,.func=pipeAddNode},
	{.op=PIPE_NODE_LIST			,.func=pipeNodeList},
	{.op=PIPE_NODE_CONNECT		,.func=pipeNodeConnect},
	{.op=PIPE_NODE_DISCONNECT	,.func=pipeNodeDisConnect},
	{.op=PIPE_NODE_SET_CONST	,.func=pipeNodeSetConst},
	{.op=PIPE_NODE_GET_CONST	,.func=pipeNodeGetConst},
	{.op=PIPE_GET_NODE_NAME_LIST,.func=pipeGetNodeNameList},
	{.op=PIPE_GET_PIPE_NAME_LIST,.func=pipeGetPipeNameList},
	{.op=PIPE_SAVE				,.func=pipeSave},
	{.op=PIPE_LOAD				,.func=pipeLoad},
	{.op=PIPE_TIMER_RUN			,.func=pipeTimerRun},
	{.op=PIPE_TIMER_STOP		,.func=pipeTimerStop},
	{.op=PIPE_TIMER_SET			,.func=pipeTimerSet},
	{.op=PIPE_TIMER_GET			,.func=pipeTimerGet}
};

//const value
static const char logRootPath[] = "./Logs";

//global value
static int pid;
static int fd[2];
static char* logFolder;
static nodeData** activeNodeList = NULL;
static nodeData** inactiveNodeList = NULL;
static shm_key wakeupNodeArray;

int nodeSystemInit(uint8_t isNoLog){
	//set logfile
	if(!logFile)
		logFile = stdout;
	
	//check
	if(systemSettingMemory != NULL){
		debugPrintf("%s(): function has already been executed",__func__);
		return -1;
	}

	//malloc global memory
	CHECK(0,shareMemoryGenerate(sizeof(nodeSystemEnv),&systemSettingKey));
	CHECK(0,shareMemoryOpen(&systemSettingKey,0));
	systemSettingMemory = systemSettingKey.shmMap;

	//calc timezone
	time_t t = time(NULL);
	struct tm lt = {0};
	localtime_r(&t, &lt);
	systemSettingMemory->timeOffset = lt.tm_gmtoff;

	//set env data
	systemSettingMemory->isNoLog = isNoLog;
	systemSettingMemory->period = 1000;
	
	//create logDirPath
	char logFilePath[PATH_MAX];
	sprintf(logFilePath,"%s/%s",logRootPath,getRealTimeStr());

	//createDir
	if(!systemSettingMemory->isNoLog){
		mkdir(logRootPath,0777);
		if(mkdir(logFilePath,0777) != 0){
			debugPrintf("%s(): mkdir(): %s",__func__,strerror(errno));
			return -1;
		}
		logFolder = realpath(logFilePath,NULL);
	}
	
	
	int in[2],out[2];
	// open pipe 
	if(pipe(in) != 0 || pipe(out) != 0){
		debugPrintf("%s(): pipe(): %s",__func__,strerror(errno));
		return -1;
	}

	//set nonblock
	fcntl(in[0] ,F_SETFL,fcntl(in[0] ,F_GETFL) | O_NONBLOCK);
	fcntl(in[1] ,F_SETFL,fcntl(in[1] ,F_GETFL) | O_NONBLOCK);
	fcntl(out[0],F_SETFL,fcntl(out[0],F_GETFL) | O_NONBLOCK);
	fcntl(out[1],F_SETFL,fcntl(out[1],F_GETFL) | O_NONBLOCK);

		
	// fork program
	pid = fork();
	if(pid == -1){
		debugPrintf("%s(): fork(): %s",__func__,strerror(errno));
		return -1;
	}

	if(pid != 0){
		//parent
		fd[0] = out[0];
		fd[1] = in[1];
		close(in[0]);
		close(out[1]);
	}else{
		//child
		fd[0] = in[0];
		fd[1] = out[1];
		close(out[0]);
		close(in[1]);

		//init list
		activeNodeList = LINEAR_LIST_CREATE(nodeData*);
		inactiveNodeList = LINEAR_LIST_CREATE(nodeData*);

		//fork timer thread
		shareMemoryGenerate(sizeof(int)*4096,&wakeupNodeArray);
		shareMemoryOpen(&wakeupNodeArray,0);
		memset(wakeupNodeArray.shmMap,0,sizeof(int)*4096);
		pid = fork();
		if(pid == 0){
			pid = getppid();
			struct timespec interval = {};
			int* pidList = wakeupNodeArray.shmMap;
		
			while(kill(pid,0) == 0){
				interval.tv_sec = (systemSettingMemory->period * 1000000LL)/1000000000LL;
				interval.tv_nsec = (systemSettingMemory->period * 1000000LL)%1000000000LL;

				shareMemoryLock(&wakeupNodeArray);
				if(pidList[0]){
					int i;
					for(i = 1;pidList[i] != 0;i++){
						kill(pidList[i],SIGCONT);
					}
					shareMemoryUnLock(&wakeupNodeArray);
					nanosleep(&interval,NULL);
				}else{
					shareMemoryUnLock(&wakeupNodeArray);
					nanosleep(&interval,NULL);
				}
			}
			shareMemoryDeleate(&wakeupNodeArray);
			exit(0);
		}else if(pid < 0){
			exit(1);
		}

		//set log file
		logFile = NULL;
		if(!systemSettingMemory->isNoLog){
			//create log file path
			sprintf(logFilePath,"%s/NodeSystem.txt",logFolder);
			logFile = fopen(logFilePath,"w");

			//failed open logFile
			if(logFile == NULL){
				exit(-1);
			}

			debugPrintf("%s(): nodeSystem is activate.",__func__);
		}

		//loop
		int parent = getppid();
		while(kill(parent,0) == 0){
			nodeSystemLoop();
		}

		//remove mem
		nodeData** itr;
		LINEAR_LIST_FOREACH(activeNodeList,itr){
			nodeDeleate(*itr);
		}

		//close
		if(logFile)
			fclose(logFile);
		
		exit(0);
	}

	return 0;
}


int nodeSystemAddNode(char* path,char** args){

	//check argment
	if(!path || !args){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}
	
	//count args
	uint16_t c = 0;
	while(args[c] != NULL)
		c++;

	//send message head
	uint8_t head = PIPE_ADD_NODE;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send execute path
	fileWriteStr(fd[1],path);

	//send args count
	fileWrite(fd[1],&c,sizeof(c));

	//send args
	int i;
	for(i = 0;i < c;i++){
		fileWriteStr(fd[1],args[i]);
	}

	//wait result
	int res = 0;
	fileRead(fd[0],&res,sizeof(res));

	return res;
}


void nodeSystemPrintNodeList(int* argc,char** args){

	//send message head
	uint8_t head = PIPE_NODE_LIST;
	fileWrite(fd[1],&head,sizeof(head));

	nodeData** itr;
	//print active list
	fprintf(stdout,
					"--------------------active node list--------------------\n");

	uint16_t activeNodeCount;
	fileRead(fd[0],&activeNodeCount,sizeof(activeNodeCount));
	

	int i;
	for(i = 0;i < activeNodeCount;i++){
		char name[PATH_MAX];
		char filePath[PATH_MAX];
		size_t len;

		//print head
		fprintf(stdout,"\n"
					"|------------------------------------------");
		
		//receive node name
		fileReadStr(fd[0],name,sizeof(name));

		//receive node file
		fileReadStr(fd[0],filePath,sizeof(filePath));

		//print node anme and path
		fprintf(stdout,"\n"
					"|\tname: %s\n"
					"|\tpath: %s\n"
					,name,filePath);
		
		//receive pipe count
		uint16_t pipeCount;
		fileRead(fd[0],&pipeCount,sizeof(pipeCount));

		int j;
		for(j = 0;j < pipeCount;j++){
			nodePipe data;
			char nodeName[PATH_MAX];
			char pipeName[PATH_MAX];

			//receive pipe name
			fileReadStr(fd[0],pipeName,sizeof(pipeName));

			//receive node type
			fileRead(fd[0],&data.type,sizeof(data.type));
			fileRead(fd[0],&data.unit,sizeof(data.unit));
			fileRead(fd[0],&data.length,sizeof(data.length));

			//print pipe name and type
			fprintf(stdout,"|\n"
					"|\tpipeName:%s\n"
					"|\tpipeType:%s\n"
					"|\tpipeUnit:%s\n"
					"|\tpipeLength:%d\n"
					,pipeName,NODE_PIPE_TYPE_STR[data.type]
					,NODE_DATA_UNIT_STR[data.unit],(int)data.length);
			
			//receive state
			uint8_t isConnected = 0;
			fileRead(fd[0],&isConnected,sizeof(isConnected));

			if(isConnected){
				//recive connect node and pipe name
				fileReadStr(fd[0],nodeName,sizeof(nodeName));
				fileReadStr(fd[0],pipeName,sizeof(pipeName));

				//print pipe name and type
				fprintf(stdout,
					"|\tConnected to %s of %s\n"
					,pipeName,nodeName);
			}else if(data.type == NODE_PIPE_IN){
				//print pipe name and type
				fprintf(stdout,
					"|\tnot Connected\n");
			}

		}

		fprintf(stdout,
				"|\n"
				"|------------------------------------------\n");
	}

	fprintf(stdout,"\n"
				"--------------------------------------------------------\n");
}

int nodeSystemConnect(char* const inNode,char* const inPipe,char* const outNode,char* const outPipe){
	
	//send message head
	uint8_t head = PIPE_NODE_CONNECT;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send in pipe
	fileWriteStr(fd[1],inNode);
	fileWriteStr(fd[1],inPipe);

	//send out pipe
	fileWriteStr(fd[1],outNode);
	fileWriteStr(fd[1],outPipe);

	//wait result
	int res = 0;
	fileRead(fd[0],&res,sizeof(res));

	return res;
}

int nodeSystemDisConnect(char* const inNode,char* const inPipe){
	
	//send message head
	uint8_t head = PIPE_NODE_DISCONNECT;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send  in pipe
	fileWriteStr(fd[1],inNode);
	fileWriteStr(fd[1],inPipe);

	//wait result
	int res = 0;
	fileRead(fd[0],&res,sizeof(res));

	return res;
}

int nodeSystemSetConst(char* const constNode,char* const constPipe,int valueCount,char** setValue){
	
	//send message head
	uint8_t head = PIPE_NODE_SET_CONST;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send const pipe
	fileWriteStr(fd[1],constNode);
	fileWriteStr(fd[1],constPipe);

	//send const len
	fileWrite(fd[1],&valueCount,sizeof(valueCount));

	//get result
	int res = 0;
	fileRead(fd[0],&res,sizeof(res));
	if(res != 0)
		return res;

	int i;
	for(i = 0;i < valueCount;i++){
		//send value
		fileWriteStr(fd[1],setValue[i]);
	}

	//get result
	fileRead(fd[0],&res,sizeof(res));

	return res;
}

int nodeSystemSave(char* const path){
	
	//send message head
	uint8_t head = PIPE_SAVE;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send const pipe
	fileWriteStr(fd[1],path);

	//get result
	int res = 0;
	fileRead(fd[0],&res,sizeof(res));

	return res;
}

int nodeSystemLoad(char* const path){
	
	//open laod file
	FILE* loadFile = fopen(path,"r");
	if(loadFile == NULL){
		debugPrintf("%s(): fopen(): %s",__func__,strerror(errno));
		return -1;
	}

	//run nodes
	while(1){
		//get node path
		char nodePath[4096];
		if(fgets(nodePath,sizeof(nodePath),loadFile) != nodePath || nodePath[0] == '\n'){
			break;
		}

		//get node name
		char nodeName[4096];
		if(fgets(nodeName,sizeof(nodeName),loadFile) != nodeName || nodeName[0] == '\n'){
			debugPrintf("%s(): failed load node name",__func__);
			fclose(loadFile);
			return -1;
		}

		//print name and path
		nodePath[strlen(nodePath)-1] = '\0';
		nodeName[strlen(nodeName)-1] = '\0';
		debugPrintf("loading node \nname:%s\npath:%s",nodeName,nodePath);
		
		char* args[4] = {nodePath,"-name",nodeName,NULL};
		int code = nodeSystemAddNode(nodePath,args);
		
		if(code  != 0)
			debugPrintf("load node failed");
	};

	//connect pipe
	while(1){
		//get node path
		char nodeName[4096];
		if(fgets(nodeName,sizeof(nodeName),loadFile) != nodeName || nodeName[0] == '\n'){
			break;
		}

		//get node name
		char pipeName[4096];
		if(fgets(pipeName,sizeof(pipeName),loadFile) != pipeName || pipeName[0] == '\n'){
			debugPrintf("%s(): failed load pipe name",__func__);
			fclose(loadFile);
			return -1;
		}

		//get connect node name
		char connectNodeName[4096];
		if(fgets(connectNodeName,sizeof(connectNodeName),loadFile) != connectNodeName || connectNodeName[0] == '\n'){
			debugPrintf("%s(): failed load connect node name",__func__);
			fclose(loadFile);
			return -1;
		}

		//get connect pipe name
		char connectPipeName[4096];
		if(fgets(connectPipeName,sizeof(connectPipeName),loadFile) != connectPipeName || connectPipeName[0] == '\n'){
			debugPrintf("%s(): failed load connect pipe name",__func__);
			fclose(loadFile);
			return -1;
		}

		//print name and path
		nodeName[strlen(nodeName)-1] = '\0';
		pipeName[strlen(pipeName)-1] = '\0';
		connectNodeName[strlen(connectNodeName)-1] = '\0';
		connectPipeName[strlen(connectPipeName)-1] = '\0';
		debugPrintf("load pipe connection \ninNode:%s\ninPipe:%s\noutNode:%s\noutPipe:%s",nodeName,pipeName,connectNodeName,connectPipeName);
		
		int code = nodeSystemConnect(nodeName,pipeName,connectNodeName,connectPipeName);
		
		if(code  != 0)
			debugPrintf("load node connection failed");
	};

	//set const
	while(1){
		//get node path
		char nodeName[4096];
		if(fgets(nodeName,sizeof(nodeName),loadFile) != nodeName || nodeName[0] == '\n'){
			break;
		}

		//get pipe name
		char pipeName[4096];
		if(fgets(pipeName,sizeof(pipeName),loadFile) != pipeName || pipeName[0] == '\n'){
			debugPrintf("%s(): failed load const node name",__func__);
			fclose(loadFile);
			return -1;
		}

		//get data length
		char dataLength[4096];
		if(fgets(dataLength,sizeof(dataLength),loadFile) != dataLength || dataLength[0] == '\n'){
			debugPrintf("%s(): failed load const array length",__func__);
			fclose(loadFile);
			return -1;
		}

		int size;
		sscanf(dataLength,"%d",&size);

		//get connect pipe name
		void* mem = malloc(size);
		fread(mem,size,1,loadFile);


		//print name and path
		nodeName[strlen(nodeName)-1] = '\0';
		pipeName[strlen(pipeName)-1] = '\0';

		debugPrintf("load const pipe \nNode:%s\nPipe:%s",nodeName,pipeName);
	
		//send message head
		uint8_t head = PIPE_LOAD;
		fileWrite(fd[1],&head,sizeof(head));

		//send node name and piepe name
		fileWriteStr(fd[1],nodeName);
		fileWriteStr(fd[1],pipeName);

		//send data
		fileWrite(fd[1],&size,sizeof(size));
		fileWrite(fd[1],mem,size);

		//get result
		int code;
		fileRead(fd[0],&code,sizeof(code));

		free(mem);

		if(code  != 0)
			debugPrintf("load const array failed");
	};

	fclose(loadFile);

	return 0;
}

char** nodeSystemGetConst(char* const constNode,char* const constPipe,int* retCode){
	
	//send message head
	uint8_t head = PIPE_NODE_GET_CONST;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send const pipe
	fileWriteStr(fd[1],constNode);
	fileWriteStr(fd[1],constPipe);

	//receive count
	fileRead(fd[0],retCode,sizeof(*retCode));
	if(retCode < 0)
		return NULL;

	//malloc mem
	char** values = malloc(sizeof(char*) * *retCode);

	int i;
	for(i = 0;i < *retCode;i++){
		//receive value
		values[i] = malloc(256);
		fileReadStr(fd[0],values[i],256);
	}

	return values;
}

void nodeSystemTimerRun(){

	fprintf(stdout,"Timer start\n");
	
	//send message head
	uint8_t head = PIPE_TIMER_RUN;
	fileWrite(fd[1],&head,sizeof(head));
}

void nodeSystemTimerStop(){

	fprintf(stdout,"Timer stop\n");
	
	//send message head
	uint8_t head = PIPE_TIMER_STOP;
	fileWrite(fd[1],&head,sizeof(head));
}

void nodeSystemTimerSet(long period){
	fprintf(stdout,"Timer period set to %ldms\n",period);

	//send message head
	uint8_t head = PIPE_TIMER_SET;
	fileWrite(fd[1],&head,sizeof(head));
	
	//send period
	fileWrite(fd[1],&period,sizeof(period));
}

void nodeSystemTimerGet(){
	long period;

	//send message head
	uint8_t head = PIPE_TIMER_GET;
	fileWrite(fd[1],&head,sizeof(head));

	//receive period
	fileRead(fd[0],&period,sizeof(period));

	fprintf(stdout,"Timer period is %ldms\n",period);
}

char** nodeSystemGetNodeNameList(int* counts){
	
	//send message head
	uint8_t head = PIPE_GET_NODE_NAME_LIST;
	fileWrite(fd[1],&head,sizeof(head));
	
	//receive node count
	uint16_t nodeCounts;
	fileRead(fd[0],&nodeCounts,sizeof(nodeCounts));
	
	//malloc name list
	char** names = malloc(sizeof(char*) * nodeCounts);

	int i;
	for(i = 0;i < nodeCounts;i++){
		names[i] = malloc(PATH_MAX);
		fileReadStr(fd[0],names[i],PATH_MAX);
	}

	*counts = nodeCounts;
	return names;
}

char** nodeSystemGetPipeNameList(char* nodeName,int* counts){
	
	//send message head
	uint8_t head = PIPE_GET_PIPE_NAME_LIST;
	fileWrite(fd[1],&head,sizeof(head));

	//send node name
	fileWriteStr(fd[1],nodeName);
	
	//receive pipe count
	uint16_t pipeCounts;
	fileRead(fd[0],&pipeCounts,sizeof(pipeCounts));
	
	//malloc name list
	char** names = malloc(sizeof(char*) * pipeCounts);

	int i;
	for(i = 0;i < pipeCounts;i++){
		names[i] = malloc(PATH_MAX);
		fileReadStr(fd[0],names[i],PATH_MAX);
	}

	counts[0] = pipeCounts;
	return names;
}

static void nodeSystemLoop(){
	//check inactive nodes
	nodeData** itr;
	LINEAR_LIST_FOREACH(inactiveNodeList,itr){
		int ret = nodeBegin(*itr);
		if(ret == 0){
			nodeData* data = *itr;
			LINEAR_LIST_ERASE(itr);
			LINEAR_LIST_PUSH(activeNodeList,data);

			int i;
			int* pidList = wakeupNodeArray.shmMap;
			for(i = 1;pidList[i] != 0;i++){
			}
			shareMemoryLock(&wakeupNodeArray);
			pidList[i] = (*itr)->pid;
			shareMemoryUnLock(&wakeupNodeArray);
		}else if(ret < 0){
			//kill
			kill((*itr)->pid,SIGTERM);
			//deleate node
			nodeDeleate(*itr);
			//deleate from list
			LINEAR_LIST_ERASE(itr);
			break;
		}
	}

	//check active node
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		//check process
		if(kill((*itr)->pid,0)){
			//deleate node
			nodeDeleate(*itr);
			//deleate from list
			LINEAR_LIST_ERASE(itr);
		}
	}

	//message from parent 
	uint8_t head;
	if(fileReadWithTimeOut(fd[0],&head,sizeof(head),1) == sizeof(head)){
		//serch table
		int i;
		for(i = 0;i < (sizeof(opTable)/sizeof(opTable[0]));i++){
			if(head == opTable[i].op){
				opTable[i].func();
				break;
			}
		}
	}else{
		//timer
		static const struct timespec req = {.tv_sec = 0,.tv_nsec = 1000*1000};
		nanosleep(&req,NULL);
	}
}

static int popenRWasNonBlock(const char const * command,int* fd){

	int pipeTx[2];
	int pipeRx[2];
	int pipeErr[2];
	//create pipe
	if(pipe(pipeTx) < 0 || pipe(pipeRx) < 0 || pipe(pipeErr) < 0){
		debugPrintf("%s(): pipe(): %s",__func__,strerror(errno));
		return -1;
	}

	//fork
	int process = fork();
	if(process == -1){
		debugPrintf("%s(): fork(): %s",__func__,strerror(errno));
		return -1;
	}

 	if(process == 0)
	{//child
	 	//dup pipe
		close(pipeTx[1]);
		close(pipeRx[0]);
		close(pipeErr[0]);
		dup2(pipeTx[0], STDIN_FILENO);
		dup2(pipeRx[1], STDOUT_FILENO);
		dup2(pipeErr[1], STDERR_FILENO);
		close(pipeTx[0]);
		close(pipeRx[1]);
		close(pipeErr[1]);
		
		execl(command,command,NULL);

		exit(EXIT_SUCCESS);
	}
	else
	{//parent
		//close pipe
		close(pipeTx[0]);
		close(pipeRx[1]);
		close(pipeErr[1]);
		fd[0] = pipeRx[0];
		fd[1] = pipeTx[1];
		fd[2] = pipeErr[0];

		//set nonblock
		fcntl(fd[0],F_SETFL,fcntl(fd[0],F_GETFL) | O_NONBLOCK);
		fcntl(fd[1],F_SETFL,fcntl(fd[1],F_GETFL) | O_NONBLOCK);
		fcntl(fd[2],F_SETFL,fcntl(fd[2],F_GETFL) | O_NONBLOCK);

		return process;
	}
}

static int nodeBegin(nodeData* node){
	uint32_t header_buffer;
	
	int ret = fileReadWithTimeOut(node->fd[0],&header_buffer,sizeof(header_buffer),1);

	if(ret == 0){
		return 1;
	}
	else if((ret != sizeof(header_buffer)) || (header_buffer != _node_begin_head)){
		debugPrintf("%s: [%s]: Failed recive header",__func__,node->name);
		return -1;
	}

	//give pipe
	int i;
	for(i = 0;i < node->pipeCount;i++){
		if(node->pipes[i].type != NODE_PIPE_IN){
			size_t memSize = NODE_DATA_UNIT_SIZE[node->pipes[i].unit] * node->pipes[i].length + 1;

			//get share memory
			if(shareMemoryGenerate(memSize,&node->pipes[i].shm) < 0){
				debugPrintf("%s(): [%s.%s]: failed generate share memory",__func__,node->name,node->pipes[i].pipeName);
				return -1;
			}

			
			fileWrite(node->fd[1],&node->pipes[i].shm.semId,sizeof(node->pipes[i].shm.semId));
			fileWrite(node->fd[1],&node->pipes[i].shm.shmId,sizeof(node->pipes[i].shm.shmId));
		}
	}

	//receive eof
	ret = fileReadWithTimeOut(node->fd[0],&header_buffer,sizeof(header_buffer),1000000LL);
	if((ret != sizeof(header_buffer)) || (header_buffer != _node_begin_eof)){
		debugPrintf("%s(): [%s]: Failed recive eof",__func__,node->name);
		return -1;
	}else if(header_buffer != _node_begin_eof){
		if(!systemSettingMemory->isNoLog)
		return -2;
	}

	debugPrintf("%s(): [%s]: Node is activated",__func__,node->name);
	return 0;
}

static void nodeDeleate(nodeData* node){
	//releace mem
	int i;
	for(i = 0;i < node->pipeCount;i++){
		//free
		free(node->pipes[i].pipeName);

		//check type
		if(node->pipes[i].type == NODE_PIPE_IN){
			//close
			if(shareMemoryClose(&node->pipes[i].shm) != 0){
				debugPrintf("%s(): failed deleate memory.",__func__);
			}
		}else{
			//deleate
			if(shareMemoryDeleate(&node->pipes[i].shm) != 0){
				debugPrintf("%s(): failed deleate memory.",__func__);
			}
		}
	}

	//remove from wakeup List
	int  f;
	int* pidList = wakeupNodeArray.shmMap;
	for(i = 1,f = 0;pidList[i] != 0;i++){
		if(f){
			pidList[i - 1] = pidList[i];
			pidList[i] = 0;
		}else{
			if(pidList[i] == node->pid){
				pidList[i] = 0;
				f = 1;
			}
		}
	}
	
	//free
	free(node->pipes);
	if((node->name < node->filePath) || (node->name > (node->filePath+strlen(node->filePath))))
		free(node->name); 
	free(node->filePath); 
	free(node);

}

static int receiveNodeProperties(nodeData* node){
	char recvBuffer[1024];
	
	//receive header
	int res = fileReadWithTimeOut(node->fd[0],recvBuffer,sizeof(_node_init_head),1000000LL);
	if((res != sizeof(_node_init_head))|| ((typeof(_node_init_head)*)recvBuffer)[0] != _node_init_head){
		debugPrintf("%s(): Received header is invalid",__func__);
		return -1;
	}

	//read system Env
	fileWrite(node->fd[1],&systemSettingKey.semId,sizeof(int));
	fileWrite(node->fd[1],&systemSettingKey.shmId,sizeof(int));

	//send path
	char path[PATH_MAX];
	sprintf(path,"%s/%s.txt",logFolder,node->name);
	fileWriteStr(node->fd[1],path);

	//receive pipe count
	if(fileReadWithTimeOut(node->fd[0],recvBuffer,sizeof(uint16_t),1000000LL) != sizeof(uint16_t)){
		debugPrintf("%s(): Failed receive pipe count",__func__);
		return -1;
	}	

	//malloc pipes buffer
	node->pipeCount = ((uint16_t*)recvBuffer)[0];
	node->pipes = malloc(sizeof(nodePipe)*node->pipeCount);
	memset(node->pipes,0,sizeof(nodePipe)*node->pipeCount);
	
	//receive pipe
	int i;
	for(i = 0;i < node->pipeCount;i++){
		//type
		if(fileReadWithTimeOut(node->fd[0],&node->pipes[i].type,sizeof(uint8_t),1000000LL) != sizeof(uint8_t)){
			debugPrintf("%s(): Failed receive pipe type",__func__);
			return -1;
		}	

		//unit
		if(fileReadWithTimeOut(node->fd[0],&node->pipes[i].unit,sizeof(uint8_t),1000000LL) != sizeof(uint8_t)){
			debugPrintf("%s(): Failed receive pipe unit",__func__);
			return -1;
		}	
		
		//length
		if(fileReadWithTimeOut(node->fd[0],&node->pipes[i].length,sizeof(uint16_t),1000000LL) != sizeof(uint16_t)){
			debugPrintf("%s(): Failed receive array length",__func__);
			return -1;
		}	
		
		//name
		res = fileReadStrWithTimeOut(node->fd[0],recvBuffer,sizeof(recvBuffer),1000000LL);
		if(res == 0 || recvBuffer[res - 1] != '\0'){
			debugPrintf("%s(): Failed receive pipe name",__func__);
			return -1;
		}
		node->pipes[i].pipeName = malloc(res);
		strcpy(node->pipes[i].pipeName,recvBuffer);
	
		debugPrintf("%s(): Success received pipe data\n"
				"--------------------------------------\n"
				"PipeName: %s\n"
				"PipeType: %s\n"
				"PipeUnit: %s\n"
				"ArraySize: %d\n"
				"--------------------------------------",
				__func__,
				node->pipes[i].pipeName,
				NODE_PIPE_TYPE_STR[node->pipes[i].type],
				NODE_DATA_UNIT_STR[node->pipes[i].unit],
				node->pipes[i].length);
	}
	
	//recive eof
	res = fileReadWithTimeOut(node->fd[0],recvBuffer,sizeof(_node_init_eof),1000000LL);
	if((res != sizeof(_node_init_eof)) || (((typeof(_node_init_eof)*)recvBuffer)[0] != _node_init_eof)){
		debugPrintf("%s(): Failed receive eof",__func__);
		return -1;
	}

	debugPrintf("%s(): Success received node data",__func__);
	return 0;
}

static void pipeAddNode(){
	//init struct
	nodeData* data = malloc(sizeof(nodeData));
	memset(data,0,sizeof(nodeData));

	//get path
	char path[PATH_MAX];
	fileReadStr(fd[0],path,sizeof(path));
	data->filePath = malloc(strlen(path)+1);
	strcpy(data->filePath,path);
	data->name = strrchr(data->filePath,'/');
	if(data->name)
		data->name++;
	else
		data->name = data->filePath;

	//args count
	uint16_t argsCount;
	fileRead(fd[0],&argsCount,sizeof(argsCount));

	//load args
	char** args = malloc(sizeof(char*)*argsCount);
	int i;
	for(i = 0;i < argsCount;i++){
		args[i] = malloc(PATH_MAX);
		fileReadStr(fd[0],args[i],PATH_MAX);
		char* newPtr = realloc(args[i],strlen(args[i])+1);
		if(newPtr != NULL)
			args[i] = newPtr;
	}
	
	//do args
	for(i = 0;i < (argsCount-1);i++){
		if(strcmp(args[i],"-name") == 0){
			free(args[i++]);
			data->name = args[i];
		}else{
			free(args[i]);
		}
	}

	//free
	free(args);

	//check name conflict
	int f = 0;
	nodeData** itr;
	LINEAR_LIST_FOREACH(inactiveNodeList,itr){
		if(strcmp((*itr)->name,data->name) == 0)
			f =1;
		else if(f)
			break;
	}
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,data->name) == 0)
			f =1;
		else if(f)
			break;
	}

	//name conflict
	if(f){
		debugPrintf("%s(): name conflict",__func__);

		//free
		if((data->name < data->filePath) || (data->name > (data->filePath+strlen(data->filePath))))
			free(data->name);
		free(data->filePath);
		free(data);

		int res = -1;
		fileWrite(fd[1],&res,sizeof(res));
		return;
	}

	//execute program
	data->pid = popenRWasNonBlock(data->filePath,data->fd);
	if(data->pid < 0){
		debugPrintf("%s(): Failed execute file",__func__);
		
		//free
		if((data->name < data->filePath) || (data->name > (data->filePath+strlen(data->filePath))))
			free(data->name);
		free(data->filePath);
		free(data);

		int res = -1;
		fileWrite(fd[1],&res,sizeof(res));
		return;
	}

	
	//load properties
	if(receiveNodeProperties(data)){
		kill(data->pid,SIGTERM);
		//free
		if((data->name < data->filePath) || (data->name > (data->filePath+strlen(data->filePath))))
			free(data->name);
		free(data->filePath);
		free(data);

		int res = -1;
		fileWrite(fd[1],&res,sizeof(res));
		return;
	}
	else
		LINEAR_LIST_PUSH(inactiveNodeList,data);

	debugPrintf("a");

	int res = 0;
	fileWrite(fd[1],&res,sizeof(res));	
}

static void pipeNodeList(){
	uint16_t nodeCount = 0;
	nodeData** itr;
	
	//get node count
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		nodeCount++;
	}

	//send node count
	fileWrite(fd[1],&nodeCount,sizeof(nodeCount));
	
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		//send node name
		fileWriteStr(fd[1],(*itr)->name);
		
		//send file path
		fileWriteStr(fd[1],(*itr)->filePath);

		//send pipe count
		fileWrite(fd[1],&(*itr)->pipeCount,sizeof((*itr)->pipeCount));
		
		int i;
		for(i = 0;i < (*itr)->pipeCount;i++){
			//semd pipe data
			fileWriteStr(fd[1],(*itr)->pipes[i].pipeName);
			fileWrite(fd[1],&(*itr)->pipes[i].type,sizeof((*itr)->pipes[i].type));
			fileWrite(fd[1],&(*itr)->pipes[i].unit,sizeof((*itr)->pipes[i].unit));
			fileWrite(fd[1],&(*itr)->pipes[i].length,sizeof((*itr)->pipes[i].length));

			//send state
			uint8_t isConnected = (*itr)->pipes[i].connectPipe != NULL;
			fileWrite(fd[1],&isConnected,sizeof(isConnected));

			if(isConnected){
				//send connect node and pipe
				fileWriteStr(fd[1],(*itr)->pipes[i].connectNode);
				fileWriteStr(fd[1],(*itr)->pipes[i].connectPipe);
			}
		}
	}
}

static void pipeNodeConnect(){
	char inNode[PATH_MAX];
	char inPipe[PATH_MAX];
	char outNode[PATH_MAX];
	char outPipe[PATH_MAX];
	
	//receive in pipe
	fileReadStr(fd[0],inNode,PATH_MAX);
	fileReadStr(fd[0],inPipe,PATH_MAX);

	//receive out pipe
	fileReadStr(fd[0],outNode,PATH_MAX);
	fileReadStr(fd[0],outPipe,PATH_MAX);

	//finde pipe
	nodePipe* in = NULL,*out = NULL;
	nodeData *node_in = NULL,*node_out = NULL;
	uint16_t pipe_in = 0;
	shm_key* outputMem = NULL;

	nodeData** itr;
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,inNode) == 0){
			node_in = *itr;
			//find in pipe
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if(strcmp((*itr)->pipes[i].pipeName,inPipe) == 0){
					pipe_in = i;
					in = &(*itr)->pipes[i];
					break;
				}
			}
		}

		if(strcmp((*itr)->name,outNode) == 0){
			node_out = *itr;
			//find out pipe
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if(strcmp((*itr)->pipes[i].pipeName,outPipe) == 0){
					outputMem = &(*itr)->pipes[i].shm;
					out = &(*itr)->pipes[i];
					break;
				}
			}
		}
	}

	int res = 0;
	if(in == NULL || out == NULL){
		debugPrintf("%s(): Pipe not found",__func__);
		res = -1;
	}else if(in->type != NODE_PIPE_IN || out->type != NODE_PIPE_OUT || in->unit != out->unit || in->length != out->length){
		debugPrintf("%s(): Pipe type is invalid",__func__);
		res = -1;
	}else{
		fileWrite(node_in->fd[1],&pipe_in,sizeof(pipe_in));
		fileWrite(node_in->fd[1],&outputMem->semId,sizeof(outputMem->semId));
		fileWrite(node_in->fd[1],&outputMem->shmId,sizeof(outputMem->shmId));
		in->connectNode = node_out->name;
		in->connectPipe = out->pipeName;

		debugPrintf("%s(): Connect %s %s to %s %s",__func__,inNode,inPipe,outNode,outPipe);
	}

	//send result
	fileWrite(fd[1],&res,sizeof(res));
}

static void pipeNodeDisConnect(){
	char inNode[PATH_MAX];
	char inPipe[PATH_MAX];
	
	//receive in pipe
	fileReadStr(fd[0],inNode,PATH_MAX);
	fileReadStr(fd[0],inPipe,PATH_MAX);

	//finde pipe
	nodePipe* in = NULL;
	nodeData *node_in = NULL;
	uint16_t pipe_in = 0;
	shm_key outputMem = {};

	nodeData** itr;
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,inNode) == 0){
			node_in = *itr;
			//find in pipe
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if(strcmp((*itr)->pipes[i].pipeName,inPipe) == 0){
					pipe_in = i;
					in = &(*itr)->pipes[i];
					break;
				}
			}
		}
	}

	int res = 0;
	if(in == NULL){
		debugPrintf("%s(): Pipe not found",__func__);
		res = -1;
	}else if(in->type != NODE_PIPE_IN){
		debugPrintf("%s(): Pipe type is invalid",__func__);
		res = -1;
	}else{
		fileWrite(node_in->fd[1],&pipe_in,sizeof(pipe_in));
		fileWrite(node_in->fd[1],&outputMem.semId,sizeof(outputMem.semId));
		fileWrite(node_in->fd[1],&outputMem.shmId,sizeof(outputMem.shmId));
		in->connectNode = NULL;
		in->connectPipe = NULL;

		debugPrintf("%s(): Disconnect %s %s",__func__,inNode,inPipe);
	}

	//send result
	write(fd[1],&res,sizeof(res));
}

static void pipeNodeSetConst(){
	char constNode[PATH_MAX];
	char constPipe[PATH_MAX];
	
	//receive in pipe
	fileReadStr(fd[0],constNode,PATH_MAX);
	fileReadStr(fd[0],constPipe,PATH_MAX);

	//recive value count
	int count;
	fileRead(fd[0],&count,sizeof(count));

	//finde pipe
	nodePipe* pipe_const = NULL;

	nodeData** itr;
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,constNode) == 0){
			//find in pipe
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if(strcmp((*itr)->pipes[i].pipeName,constPipe) == 0){
					pipe_const = &(*itr)->pipes[i];
					break;
				}
			}
		}
	}

	int res = 0;
	if(pipe_const == NULL){
		debugPrintf("%s(): Pipe not found",__func__);
		res = -1;
	}else if(pipe_const->type != NODE_PIPE_CONST || pipe_const->length != count){
		debugPrintf("%s(): Pipe type is invalid",__func__);
		res = -1;
	}

	//send result
	fileWrite(fd[1],&res,sizeof(res));

	if(res == 0){
		uint16_t size = NODE_DATA_UNIT_SIZE[pipe_const->unit];
		void* tmpBuffer = malloc(size*pipe_const->length);
		int flag = 1;

		char constValueStr[PATH_MAX];
		//read data
		switch(pipe_const->unit){
			case NODE_UNIT_CHAR:{
				int i;
				for(i = 0;i < count;i++){
					fileReadStr(fd[0],constValueStr,PATH_MAX);
					flag &= sscanf(constValueStr,"%c",&((char*)tmpBuffer)[i]);
				}
			}
			break;
			case NODE_UNIT_BOOL:{
				int i;
				for(i = 0;i < count;i++){
					fileReadStr(fd[0],constValueStr,PATH_MAX);
					int isTrue;
					flag &= sscanf(constValueStr,"%d",&isTrue);
					((uint8_t*)tmpBuffer)[i] = (isTrue != 0);
				}
			}
			break;
			case NODE_UNIT_INT8:
			case NODE_UNIT_INT16:
			case NODE_UNIT_INT32:
			case NODE_UNIT_INT64:{
				int i;
				for(i = 0;i < count;i++){
					fileReadStr(fd[0],constValueStr,PATH_MAX);
					long value;
					flag &= sscanf(constValueStr,"%ld",&value);
					memcpy(tmpBuffer+i*size,&value,size);
					if(value < 0 && (((-1l)<<size*8)&~value))
						flag = 0;
					else if(value > 0 && ((-1l)<<size*8)&value)
						flag = 0;
				}
			}
			break;
			case NODE_UNIT_UINT8:
			case NODE_UNIT_UINT16:
			case NODE_UNIT_UINT32:
			case NODE_UNIT_UINT64:{
				int i;
				for(i = 0;i < count;i++){
					fileReadStr(fd[0],constValueStr,PATH_MAX);
					unsigned long value;
					flag &= sscanf(constValueStr,"%lu",&value);
					memcpy(tmpBuffer+i*size,&value,size);
					if(((-1l)<<size*8)&value)
						flag = 0;
				}
			}
			break;
			case NODE_UNIT_FLOAT:{
				int i;
				for(i = 0;i < count;i++){
					fileReadStr(fd[0],constValueStr,PATH_MAX);
					flag &= sscanf(constValueStr,"%f",&((float*)tmpBuffer)[i]);
				}
			}
			break;
			case NODE_UNIT_DOUBLE:{
				int i;
				for(i = 0;i < count;i++){
					fileReadStr(fd[0],constValueStr,PATH_MAX);
					flag &= sscanf(constValueStr,"%lf",&((double*)tmpBuffer)[i]);
				}
			}
			break;
			
		}

		res = 0;
		if(flag == 0){
			debugPrintf("%s(): Input data is invalid",__func__);
			res = -1;
		}else{
			//cpy data
			if(shareMemoryOpen(&pipe_const->shm,0)  == 0){
				shareMemoryLock(&pipe_const->shm);
				((uint8_t*)pipe_const->shm.shmMap)[0]++;
				memcpy(pipe_const->shm.shmMap+1,tmpBuffer,size*pipe_const->length);
				shareMemoryUnLock(&pipe_const->shm);
				shareMemoryClose(&pipe_const->shm);
			}
			else{
				debugPrintf("%s(): Failed open memory",__func__);
				res = -1;
			}
		}

		//free
		free(tmpBuffer);

		//send result
		fileWrite(fd[1],&res,sizeof(res));
	}
}

static void pipeNodeGetConst(){
	char constNode[PATH_MAX];
	char constPipe[PATH_MAX];
	
	//receive in pipe
	size_t len;
	fileReadStr(fd[0],constNode,PATH_MAX);
	fileReadStr(fd[0],constPipe,PATH_MAX);

	//finde pipe
	nodePipe* pipe_const = NULL;

	nodeData** itr;
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,constNode) == 0){
			//find in pipe
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if(strcmp((*itr)->pipes[i].pipeName,constPipe) == 0){
					pipe_const = &(*itr)->pipes[i];
					break;
				}
			}
		}
	}

	int res = 0;

	if(pipe_const == NULL){
		debugPrintf("%s(): Pipe not found",__func__);
		res = -1;
	}else if(pipe_const->type != NODE_PIPE_CONST){
		debugPrintf("%s(): Pipe type is invalid",__func__);
		res = -1;
	}else if(shareMemoryOpen(&pipe_const->shm,SHM_RDONLY) != 0){
		debugPrintf("%s(): Failed open shared memory",__func__);
		res = -1;
	}else{			
		res = pipe_const->length;
	}

	//send result
	fileWrite(fd[1],&res,sizeof(res));


	//send value
	if(res > 0){
		char value[1024];
		uint16_t size = NODE_DATA_UNIT_SIZE[pipe_const->unit];
		void* memory = pipe_const->shm.shmMap;

		//read data
		switch(pipe_const->unit){
			case NODE_UNIT_CHAR:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%c",((char*)memory)[i+1]);
					fileWriteStr(fd[1],value);
				}
			}
			break;
			case NODE_UNIT_BOOL:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%d",(int)((char*)memory)[i+1]);
					fileWriteStr(fd[1],value);
				}
			}
			break;
			case NODE_UNIT_INT8:
			case NODE_UNIT_INT16:
			case NODE_UNIT_INT32:
			case NODE_UNIT_INT64:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					long num = 0;
					memcpy(&num,memory + 1 + i*size,size);

					//if num is neg fill head to 0xFF 
					int j;
					uint8_t isNeg = (num >> (size*8 - 1))&1;
					for(j = sizeof(long);j > size;j--){
						((uint8_t*)&num)[j-1] = 0xFF * isNeg;
					}

					sprintf(value,"%ld",num);
					fileWriteStr(fd[1],value);
				}
			}
			break;
			case NODE_UNIT_UINT8:
			case NODE_UNIT_UINT16:
			case NODE_UNIT_UINT32:
			case NODE_UNIT_UINT64:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					unsigned long num = 0;
					memcpy(&num,memory + 1 + i*size,size);
					sprintf(value,"%lu",num);
					fileWriteStr(fd[1],value);
				}
			}
			break;
			case NODE_UNIT_FLOAT:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%f",((float*)(memory + 1))[i]);
					fileWriteStr(fd[1],value);
				}
			}
			break;
			case NODE_UNIT_DOUBLE:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%lf",((double*)(memory + 1))[i]);
					fileWriteStr(fd[1],value);
				}
			}
			break;	
		}

		shareMemoryClose(&pipe_const->shm);
	}
}

static void pipeSave(){
	char saveFilePath[PATH_MAX];
	
	//receive in pipe
	size_t len;
	FILE* saveFile;
	nodeData ** itr;
	int res = 0;

	//receive file path
	fileReadStr(fd[0],saveFilePath,PATH_MAX);

	saveFile = fopen(saveFilePath,"w");
	if(saveFile != NULL){
		LINEAR_LIST_FOREACH(activeNodeList,itr){
			//save filepath and name
			fprintf(saveFile,"%s\n",(*itr)->filePath);
			fprintf(saveFile,"%s\n",(*itr)->name);
		}

		//insert space
		fprintf(saveFile,"\n");

		LINEAR_LIST_FOREACH(activeNodeList,itr){
			//save pipe relation
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if((*itr)->pipes[i].type == NODE_PIPE_IN &&(*itr)->pipes[i].connectPipe != NULL){
					fprintf(saveFile,"%s\n",(*itr)->name);
					fprintf(saveFile,"%s\n",(*itr)->pipes[i].pipeName);
					fprintf(saveFile,"%s\n",(*itr)->pipes[i].connectNode);
					fprintf(saveFile,"%s\n",(*itr)->pipes[i].connectPipe);
				}
			}
		}

		//insert space
		fprintf(saveFile,"\n");

		LINEAR_LIST_FOREACH(activeNodeList,itr){
			//save const data
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if((*itr)->pipes[i].type == NODE_PIPE_CONST){
					fprintf(saveFile,"%s\n",(*itr)->name);
					fprintf(saveFile,"%s\n",(*itr)->pipes[i].pipeName);
					

					void* mem;
					if(shareMemoryOpen(&(*itr)->pipes[i].shm,0) != 0){
						fprintf(saveFile,"%d\n",(*itr)->pipes[i].length*NODE_DATA_UNIT_SIZE[(*itr)->pipes[i].unit]);
						fwrite((*itr)->pipes[i].shm.shmMap+1,NODE_DATA_UNIT_SIZE[(*itr)->pipes[i].unit],(*itr)->pipes[i].length,saveFile);
						shareMemoryClose(&(*itr)->pipes[i].shm);
					}else{
						res = -1;
						debugPrintf("%s(): [%s.%s]: Failed open shared memory\n",__func__,(*itr)->name,(*itr)->pipes[i].pipeName);
					}
				}
			}
		}
		
		//insert space
		fprintf(saveFile,"\n");
		fclose(saveFile);
	}else{
		res = -1;
	}


	//send result
	fileWrite(fd[1],&res,sizeof(res));
}

static void pipeLoad(){
	char nodeName[PATH_MAX];
	char pipeName[PATH_MAX];
	int size;

	//recive node name pipe name
	fileReadStr(fd[0],nodeName,PATH_MAX);
	fileReadStr(fd[0],pipeName,PATH_MAX);

	debugPrintf("%s(): load const pipe \nNode:%s\nPipe:%s",__func__,nodeName,pipeName);
	//receive data
	fileRead(fd[0],&size,sizeof(size));
	void* mem = malloc(size);
	fileRead(fd[0],mem,size);

	//finde pipe
	nodePipe* pipe_const = NULL;

	nodeData** itr;
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,nodeName) == 0){
			//find in pipe
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if(strcmp((*itr)->pipes[i].pipeName,pipeName) == 0){
					pipe_const = &(*itr)->pipes[i];
					break;
				}
			}
		}
	}

	int res = 0;

	if(pipe_const == NULL){
		debugPrintf("%s(): Pipe not found",__func__);
		res = -1;
	}else{
		//check size
		if(size > pipe_const->length)
			size = pipe_const->length;
		
		if(shareMemoryOpen(&pipe_const->shm,0) != 0){
			debugPrintf("%s(): Failed open shared memory",__func__);
			res = -1;
		}else{
			((uint8_t*)pipe_const->shm.shmMap)[0]++;
			memcpy(pipe_const->shm.shmMap+1,mem,size);
			shareMemoryClose(&pipe_const->shm);
		}
	}

	//free
	free(mem);

	//send result
	fileWrite(fd[1],&res,sizeof(res));
}

static void pipeTimerRun(){
	shareMemoryLock(&wakeupNodeArray);
	((int*)wakeupNodeArray.shmMap)[0] = 1;
	shareMemoryUnLock(&wakeupNodeArray);
}

static void pipeTimerStop(){
	shareMemoryLock(&wakeupNodeArray);
	((int*)wakeupNodeArray.shmMap)[0] = 0;
	shareMemoryUnLock(&wakeupNodeArray);
}

static void pipeTimerSet(){	
	shareMemoryLock(&systemSettingKey);
	fileRead(fd[0],&systemSettingMemory->period,sizeof(systemSettingMemory->period));
	shareMemoryUnLock(&systemSettingKey);
}

static void pipeTimerGet(){
	fileWrite(fd[1],&systemSettingMemory->period,sizeof(systemSettingMemory->period));
}

static void pipeGetNodeNameList(){
	uint16_t nodeCount = 0;
	nodeData** itr;
	
	//get node count
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		nodeCount++;
	}

	//send node count
	fileWrite(fd[1],&nodeCount,sizeof(nodeCount));
	
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		//send node name
		fileWriteStr(fd[1],(*itr)->name);
	}
}

static void pipeGetPipeNameList(){
	uint16_t nodeCount = 0;
	nodeData** itr;
	char nodeName[PATH_MAX];
	size_t len;

	//get node name
	fileReadStr(fd[0],nodeName,PATH_MAX);
	
	//get node count
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,nodeName) == 0){
			//send pipe count
			fileWrite(fd[1],&(*itr)->pipeCount,sizeof((*itr)->pipeCount));

			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				//send pipe name
				fileWriteStr(fd[1],(*itr)->pipes[i].pipeName);
			}

			return;
		}
	}


	//if node is not found
	uint16_t zero = 0;
	fileWrite(fd[1],&zero,sizeof(zero));
}

#else

typedef struct{
	shm_key shm;
	char* pipeName;
	uint8_t count;
	uint8_t type;
	uint8_t unit;
	uint16_t length;
} _node_pipe;

static uint8_t _nodeSystemIsActive = 0;
static NODE_DEBUG_MODE _dMode = NODE_DEBUG_MESSAGE;

static uint16_t _pipe_count = 0;
static _node_pipe* _pipes = NULL;
static int _self;
static int _parent;

int nodeSystemInit(){
	//Check system state
	if(_nodeSystemIsActive){
		return -1;
	}
	

	//Set Pid
	_self = getpid();
	_parent = getppid();

	//send header
	fileWrite(STDOUT_FILENO,&_node_init_head,sizeof(_node_init_head));

	//read system Env
	fileRead(STDIN_FILENO,&systemSettingKey.semId,sizeof(int));
	fileRead(STDIN_FILENO,&systemSettingKey.shmId,sizeof(int));
	shareMemoryOpen(&systemSettingKey,SHM_RDONLY);
	systemSettingMemory = malloc(sizeof(nodeSystemEnv));
	shareMemoryLock(&systemSettingKey);
	memcpy(systemSettingMemory,systemSettingKey.shmMap,sizeof(nodeSystemEnv));
	shareMemoryUnLock(&systemSettingKey);

	if(systemSettingKey.shmMap < 0)
		return -1;

	//read log file path
	char tmp[PATH_MAX];
	fileReadStr(STDIN_FILENO,tmp,sizeof(tmp));

	if(_dMode == NODE_DEBUG_CSV){
		char* ex = strrchr(tmp,'.');
		if(ex)
			strcpy(ex+1,"csv");
	}

	//create log file
	if(!systemSettingMemory->isNoLog){
		logFile = fopen(tmp,"w");
		if(logFile == NULL){
			return -1;
		}
	}

	//send pipe count
	fileWrite(STDOUT_FILENO,&_pipe_count,sizeof(_pipe_count));

	//send pipe data
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		fileWrite(STDOUT_FILENO,&_pipes[i].type,sizeof(_pipes[i].type));
		fileWrite(STDOUT_FILENO,&_pipes[i].unit,sizeof(_pipes[i].unit));
		fileWrite(STDOUT_FILENO,&_pipes[i].length,sizeof(_pipes[i].length));
		fileWriteStr(STDOUT_FILENO,_pipes[i].pipeName);
	}
	
	//send eof
	fileWrite(STDOUT_FILENO,&_node_init_eof,sizeof(_node_init_eof));

	//Set state
	_nodeSystemIsActive = 1;

	return 0;
}

int nodeSystemAddPipe(char* const pipeName,NODE_PIPE_TYPE type,NODE_DATA_UNIT unit,uint16_t arrayLength,const void* buff){
	//check system state
	if(_nodeSystemIsActive){
		return -1;
	}

	//check pipe count
	if(_pipe_count == 0xFFFF){
		return -1;
	}
	
	//name check
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		if(strcmp(_pipes[i].pipeName,pipeName) == 0)
			return -1;
	}

	//malloc piep struct
	_node_pipe pipe = {};
	
	//cpy data
	pipe.type = type;
	pipe.unit = unit;
	pipe.length = arrayLength;
	pipe.pipeName = malloc(strlen(pipeName)+1);
	if(!pipe.pipeName){
		return -1;
	}
	strcpy(pipe.pipeName,pipeName);

	//add pipe array
	_node_pipe* tmp = _pipes;
	_pipes = realloc(_pipes,sizeof(_node_pipe)*(_pipe_count+1));
	if(!_pipes){
		_pipes = tmp;
		free(pipe.pipeName);
		return -1;
	}
	_pipes[_pipe_count] = pipe;

	//Set init value
	if(type == NODE_PIPE_CONST && buff){
		_pipes[_pipe_count].shm.shmMap = malloc(NODE_DATA_UNIT_SIZE[unit]*arrayLength);
		memcpy(_pipes[_pipe_count].shm.shmMap,buff,NODE_DATA_UNIT_SIZE[unit]*arrayLength);
	}

	return _pipe_count++;
}

int nodeSystemBegine(){
	//check system state
	if(_nodeSystemIsActive != 1){
		return -1;
	}

	//send header
	fileWrite(STDOUT_FILENO,&_node_begin_head,sizeof(_node_begin_head));

	//receive pipe data
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		//if pipe type is not PIPE_IN
		if(_pipes[i].type != NODE_PIPE_IN){
			//receive share memory
			fileRead(STDIN_FILENO,&_pipes[i].shm.semId,sizeof(_pipes[i].shm.semId));
			fileRead(STDIN_FILENO,&_pipes[i].shm.shmId,sizeof(_pipes[i].shm.shmId));
			
			//if pipe type is PIPE_CONST
			if(_pipes[i].type == NODE_PIPE_CONST){
				//attach share memory for write
				void* initVal = _pipes[i].shm.shmMap;

				//if attach success
				if(shareMemoryOpen(&_pipes[i].shm,0) != 0){
					//if initVal is not null
					if(initVal){
						//cpy init value
						memcpy(_pipes[i].shm.shmMap+1,initVal,
							NODE_DATA_UNIT_SIZE[_pipes[i].unit]*_pipes[i].length);
						//increment write counter
						((uint8_t*)_pipes[i].shm.shmMap)[0]++;
						//free
						free(initVal);
					}
					//dettach and attach sheare memory for read 
					shareMemoryClose(&_pipes[i].shm);
					shareMemoryOpen(&_pipes[i].shm,SHM_RDONLY);
				}
			}
			else{
				//attach share memory for write
				shareMemoryOpen(&_pipes[i].shm,0);
			}
			
			//if failed shmat
			if(_pipes[i].shm.shmMap == NULL){
				return -1;
			}
		}
	}
	
	//send eof
	fileWrite(STDOUT_FILENO,&_node_begin_eof,sizeof(_node_begin_eof));

	//set nonblocking
	fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) | O_NONBLOCK);

	//set state
	_nodeSystemIsActive = 2;

	return 0;
}


int nodeSystemLoop(){
	uint16_t  pipeId;
	
	//if 
	if(fileReadWithTimeOut(STDIN_FILENO,&pipeId,sizeof(pipeId),1) == sizeof(uint16_t)){

		if(_pipes[pipeId].shm.shmMap != NULL){
			if(shareMemoryClose(&_pipes[pipeId].shm) != 0)
				debugPrintf("%s(): [%s]: Failed close shared memory",__func__,_pipes[pipeId].pipeName);
		}

		_pipes[pipeId].count = 0;
		fileRead(STDIN_FILENO,&_pipes[pipeId].shm.semId,sizeof(_pipes[pipeId].shm.semId));
		fileRead(STDIN_FILENO,&_pipes[pipeId].shm.shmId,sizeof(_pipes[pipeId].shm.shmId));
		if(_pipes[pipeId].shm.shmId != 0){			
			shareMemoryOpen(&_pipes[pipeId].shm,SHM_RDONLY);

			debugPrintf("%s(): [%s]: Pipe connected",__func__,_pipes[pipeId].pipeName);
		}else{
			debugPrintf("%s(): [%s]: Pipe dissconnect",__func__,_pipes[pipeId].pipeName);
		}
	}

	shareMemoryLock(&systemSettingKey);
	memcpy(systemSettingMemory,systemSettingKey.shmMap,sizeof(*systemSettingMemory));
	shareMemoryUnLock(&systemSettingKey);

	return kill(_parent,0);
}

void nodeSystemDebugLog(char* const str){
	//check system state
	if(_nodeSystemIsActive < 1){
		return;
	}

	//check log flag
	if(systemSettingMemory->isNoLog)
		return;

	char* time = getRealTimeStr();
	size_t len = strlen(time);
	fwrite(time,1,len,logFile);

	fwrite(":",1,1,logFile);

	len = strlen(str);
	fwrite(str,1,len,logFile);

	fwrite("\n",1,1,logFile);

	fflush(logFile);
}

int nodeStstemSetDebugMode(NODE_DEBUG_MODE mode){
	//check system state
	if(_nodeSystemIsActive){
		return -1;
	}

	//change mode
	_dMode = mode;

	return 0;
}

int nodeSystemRead(int pipeID,void* buffer){
	//check system state
	if(_nodeSystemIsActive != 2){
		return -1;
	}
	
	//check pipe type
	if((_pipes[pipeID].shm.shmMap == NULL) || _pipes[pipeID].type == NODE_PIPE_OUT)
		return -1;
	
	shareMemoryLock(&_pipes[pipeID].shm);
	
	//read count
	uint8_t count = ((char*)_pipes[pipeID].shm.shmMap)[0];

	//copy data
	memcpy(buffer,_pipes[pipeID].shm.shmMap+1,NODE_DATA_UNIT_SIZE[_pipes[pipeID].unit] * _pipes[pipeID].length);
	
	shareMemoryUnLock(&_pipes[pipeID].shm);

	if(count == _pipes[pipeID].count)
			return 0;

	_pipes[pipeID].count = count;
	return 1;
}

int nodeSystemWrite(int pipeID,void* const buffer){
	//check system state
	if(_nodeSystemIsActive != 2){
		return -1;
	}
	
	//check pipe type
	if(_pipes[pipeID].type != NODE_PIPE_OUT)
		return -1;

	shareMemoryLock(&_pipes[pipeID].shm);

	//write count
	((uint8_t*)_pipes[pipeID].shm.shmMap)[0] = ++_pipes[pipeID].count;

	//copy data
	memcpy(_pipes[pipeID].shm.shmMap+1,buffer,NODE_DATA_UNIT_SIZE[_pipes[pipeID].unit] * _pipes[pipeID].length);

	shareMemoryUnLock(&_pipes[pipeID].shm);

	return 0;
}

int nodeSystemWait(){
	//check system state
	if(_nodeSystemIsActive != 2){
		return -1;
	}

	kill(_self,SIGTSTP);
}

long nodeSystemGetPeriod(){
	return systemSettingMemory->period;
}

#endif


static char* getRealTimeStr(){
	static char timeStr[64];
	static time_t befor;

	struct timespec spec;
    struct tm _tm;
	clock_gettime(CLOCK_REALTIME_COARSE, &spec);
	spec.tv_sec += systemSettingMemory->timeOffset;

	if(spec.tv_sec == befor)
		return timeStr;

	gmtime_r(&spec.tv_sec,&_tm);
	strftime(timeStr,sizeof(timeStr),"%Y-%m-%d-(%a)-%H:%M:%S",&_tm);

	befor = spec.tv_sec;
	return timeStr;
}

static int debugPrintf(const char* fmt,...){
	if(!logFile || !systemSettingMemory || systemSettingMemory->isNoLog)
		return -1;

	va_list arg_ptr;
	                                                                     
	va_start(arg_ptr, fmt);         
	int res = fprintf(logFile,"[%s] ",getRealTimeStr());
	res = vfprintf(logFile,fmt,arg_ptr);
	va_end(arg_ptr);
	fputc('\n',logFile);

	fflush(logFile);

	return res;
}

static int fileRead(int fd,void* buf,ssize_t size){
	ssize_t readCount;
	ssize_t readSize = 0;
	
	do{
		readCount = read(fd,buf + readSize,size - readSize);
		if(readCount > 0)
			readSize += readCount;
		else if(readCount == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
	
	}while(readSize != size);

	return size;
}

static int fileReadStr(int fd,char* str,ssize_t size){
	ssize_t readSize = 0;

	do{
		ssize_t res = read(fd,&str[readSize],1);
		if(res == 1)
			readSize ++;
		else if(res == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
	}while((readSize == 0 || str[readSize-1] != '\0') && readSize != size);

}

static int fileReadWithTimeOut(int fd,void* buf,ssize_t size,uint32_t usec){
	ssize_t readCount;
	ssize_t readSize = 0;
 	struct timespec target,spec;

    clock_gettime(CLOCK_REALTIME, &target);
    target.tv_nsec += 1000LL * usec;
	target.tv_sec += target.tv_nsec / 1000000000LL;
	target.tv_nsec %= 1000000000LL;
	

	do{
    	clock_gettime(CLOCK_REALTIME, &spec);
		readCount = read(fd,buf + readSize,size - readSize);
		if(readCount > 0)
			readSize += readCount;
		else if(readCount == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
	}while(readSize != size && !(target.tv_sec <= spec.tv_sec && target.tv_nsec <= spec.tv_nsec));

	return readSize;
}

static int fileReadStrWithTimeOut(int fd,char* str,ssize_t size,uint32_t usec){
	ssize_t readSize = 0;
 	struct timespec target,spec;

    clock_gettime(CLOCK_REALTIME, &target);
    target.tv_nsec += 1000LL * usec;
	target.tv_sec += target.tv_nsec / 1000000000LL;
	target.tv_nsec %= 1000000000LL;
	
	do{
    	clock_gettime(CLOCK_REALTIME, &spec);
		ssize_t res = read(fd,&str[readSize],1);
		if(res == 1)
			readSize ++;
		else if(res == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
	}while((readSize == 0 || str[readSize-1] != '\0') && readSize != size && !(target.tv_sec < spec.tv_sec && target.tv_nsec < spec.tv_nsec));

	return readSize;
}

static int fileWrite(int fd,const void* buf,ssize_t size){
	ssize_t writeCount;
	ssize_t writeSize = 0;
	
	do{
		writeCount = write(fd,buf + writeSize,size - writeSize);
		if(writeCount > 0)
			writeSize += writeCount;
		else if(writeCount == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
	
	}while(writeSize != size);

	return size;
}

static int fileWriteStr(int fd,const char* str){
	size_t len = strlen(str)+1;
	return fileWrite(fd,str,len);
}

static int shareMemoryGenerate(size_t size,shm_key* shm){
	//check argment
	if(!shm || !size){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	//generate shm
	shm->shmId = shmget(IPC_PRIVATE, size,0666);
	if(shm->shmId < 0){
		debugPrintf("%s(): shmget(): %s",__func__,strerror(errno));
		return -1;
	}
	//generate sem
	shm->semId = semget(IPC_PRIVATE, 1,0666);
	if(shm->semId < 0){
		debugPrintf("%s(): semget(): %s",__func__,strerror(errno));
		return -1;
	}

	//set sem
	if(semctl(shm->semId,0,SETVAL,1) < 0){
		debugPrintf("%s(): semctl(): %s",__func__,strerror(errno));
		return -1;
	}
	

	//set map
	shm->shmMap = NULL;

	return 0;
}

static int shareMemoryDeleate(shm_key* shm){
	//check argment
	if(!shm){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	//close
	if(shm->shmMap)
		shareMemoryClose(shm);

	//deleate shm
	if(shmctl(shm->shmId,IPC_RMID,NULL) != 0){
		debugPrintf("%s(): shmctl(): %s",__func__,strerror(errno));
		return -1;
	}

	//deleate sem
	if(semctl(shm->semId,IPC_RMID,0) != 0){
		debugPrintf("%s(): semctl(): %s",__func__,strerror(errno));
		return -1;
	}

	return 0;
}

static int shareMemoryOpen(shm_key* shm,int shmFlag){
	//check argment
	if(!shm){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	//generate shm
	shm->shmMap = shmat(shm->shmId,NULL,shmFlag);
	if(shm->shmMap < 0){
		shm->shmMap = NULL;
		debugPrintf("%s(): shmat(): %s",__func__,strerror(errno));
		return -1;
	}

	return 0;
}

static int shareMemoryClose(shm_key* shm){
	//check argment
	if(!shm || !shm->shmMap){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	//generate shm
	if(shmdt(shm->shmMap) != 0){
		debugPrintf("%s(): shmdt(): %s",__func__,strerror(errno));
		return -1;
	}

	//set map
	shm->shmMap = NULL;

	return 0;
}

static int shareMemoryRead(shm_key* shm,void* buf,size_t size){
	//check argment
	if(!shm){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	//lock
	if(shareMemoryLock(shm) != 0){
		debugPrintf("%s(): shareMemoryLock() is failed",__func__);
		return -1;
	}

	//copy
	memcpy(buf,shm->shmMap,size);

	//unlock
	if(shareMemoryUnLock(shm) != 0){
		debugPrintf("%s(): shareMemoryUnLock() is failed",__func__);
		return -1;
	}

	return 0;
}

static int shareMemoryWrite(shm_key* shm,void* buf,size_t size){
	//check argment
	if(!shm){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	//lock
	if(shareMemoryLock(shm) != 0){
		debugPrintf("%s(): shareMemoryLock() is failed",__func__);
		return -1;
	}

	//copy
	memcpy(shm->shmMap,buf,size);

	//unlock
	if(shareMemoryUnLock(shm) != 0){
		debugPrintf("%s(): shareMemoryUnLock() is failed",__func__);
		return -1;
	}

	return 0;
}

static int shareMemoryLock(shm_key* shm)
{
	static struct sembuf op = {.sem_num = 0,.sem_op = -1,.sem_flg = 0};
	
	//check argment
	if(!shm){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	if(semop(shm->semId,&op,1) == -1) {
		debugPrintf("%s(): semop(): %s",__func__,strerror(errno));
		return -1;
	}

	return 0;
}

static int shareMemoryUnLock(shm_key* shm)
{
	static struct sembuf op = {.sem_num = 0,.sem_op = 1,.sem_flg = 0};
	
	//check argment
	if(!shm){
		debugPrintf("%s(): invalid argment",__func__);
		return -1;
	}

	if(semop(shm->semId,&op,1) == -1) {
		debugPrintf("%s(): semop(): %s",__func__,strerror(errno));
		return -1;
	}

	return 0;
}