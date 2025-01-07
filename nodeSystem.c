#include "nodeSystem.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
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
#ifdef NODE_SYSTEM_HOST
#include <linear_list.h>
#endif

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
	time_t period;
} nodeSystemEnv;

//global
static shm_key systemSettingKey;
static nodeSystemEnv* systemSettingMemory = NULL;


//適当マジックナンバー　破滅的な変更のたびに変えて行く
static const uint32_t _node_init_head = 0x83DFC690;
static const uint32_t _node_init_eof  = 0x85CBADEF;
static const uint32_t _node_begin_head = 0x9067F3A2;
static const uint32_t _node_begin_eof  = 0x910AC8BB;

//local lib func
static char* getRealTimeStr();
static int fileRead(   int fd,void* buf,ssize_t size);
static int fileReadStr(int fd,char* str,ssize_t size);
static int fileReadWithTimeOut(   int fd,void* buf,ssize_t size,uint32_t usec);
static int fileReadStrWithTimeOut(int fd,char* str,ssize_t size,uint32_t usec);
static int fileWrite(   int fd,void* buf,ssize_t size);
static int fileWriteStr(int fd,char* str);

// #define NODE_SYSTEM_HOST
#ifdef NODE_SYSTEM_HOST

typedef struct{
	char* pipeName;
	uint16_t length;
	NODE_PIPE_TYPE type;
	NODE_DATA_UNIT unit;
	char* connectNode;
	char* connectPipe;
	int sID;
}nodePipe;

typedef struct{
	int pid;
	int fd[3];
	char* name;
	char* filePath;
	uint16_t pipeCount;
	nodePipe* pipes;
}nodeData;

//local func
static void nodeSystemLoop();
static int nodeBegin(nodeData* node);
static void nodeDeleate(nodeData* node);
static int sendNodeEnv(int fd,nodeData* data);
static int receiveNodeProperties(int fd,nodeData* node);
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
static void pipeNodeGetNodeNameList();
static void pipeNodeGetPipeNameList();

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
	PIPE_TIMER_STOP = 11
};

//const value
static const char logRootPath[] = "./Logs";

//global value
static int pid;
static int fd[2];
static FILE* logFile;
static char* logFolder;
static uint8_t isTimerRunning = 0;
static uint64_t timerPeriod = 1000;
static nodeData** activeNodeList = NULL;
static nodeData** inactiveNodeList = NULL;

int nodeSystemInit(uint8_t isNoLog){
	if(systemSettingMemory != NULL)
		return NODE_ERROR_ALREADY;

	int in[2],out[2];

	//malloc global memory
	systemSettingKey.shmId = shmget(IPC_PRIVATE, sizeof(nodeSystemEnv),0666);
	systemSettingKey.semId = semget(IPC_PRIVATE, 1,0666);
	systemSettingMemory = shmat(systemSettingKey.shmId,NULL,0);

	//calc timezone
	time_t t = time(NULL);
	struct tm lt = {0};
	localtime_r(&t, &lt);
	systemSettingMemory->timeOffset = lt.tm_gmtoff;

	//set log flag
	systemSettingMemory->isNoLog = isNoLog;
	
	//create logDirPath
	char logFilePath[PATH_MAX];

	strcpy(logFilePath,logRootPath);
	strcat(logFilePath,"/");
	strcat(logFilePath,getRealTimeStr());

	//createDir
	if(!systemSettingMemory->isNoLog){
		mkdir(logRootPath,0777);
		if(mkdir(logFilePath,0777) != 0){
			perror(__func__);
			return -1;
		}

		logFolder = realpath(logFilePath,NULL);
	}
	
	
	// open pipe 
	if(pipe(in) != 0 || pipe(out) != 0){
		perror(__func__);
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
		perror(__func__);
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


		if(!systemSettingMemory->isNoLog){
			//create log file path
			char logFilePath[1024];
			strcpy(logFilePath,logFolder);
			strcat(logFilePath,"/NodeSystem.txt");
			logFile = fopen(logFilePath,"w");

			//failed open logFile
			if(logFile == NULL){
				exit(-1);
			}

			fprintf(logFile,"%s:nodeSystem is activate.\n",getRealTimeStr());
			fflush(logFile);
		}

		//loop
		int parent = getppid();
		while(kill(parent,0) == 0){
			nodeSystemLoop();
		}

		//remove mem
		nodeData** itr;
		LINEAR_LIST_FOREACH(activeNodeList,itr){
			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				if((*itr)->pipes[i].type != NODE_PIPE_IN){
					int res = shmctl((*itr)->pipes[i].sID,IPC_RMID,NULL);
					if(res < 0 && !systemSettingMemory->isNoLog)
						fprintf(logFile,"%s:[%s]shmctl(IPC_RMID):%s\n",getRealTimeStr(),(*itr)->name,strerror(errno));
				}
			}
		}

		if(!systemSettingMemory->isNoLog)
			fclose(logFile);
		exit(0);
	}

	return 0;
}


int nodeSystemAddNode(char* path,char** args){
	if(path == NULL)
		return -1;

	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//count args
	uint16_t c = 0;
	while(args[c] != NULL)
		c++;

	//send message head
	uint8_t head = PIPE_ADD_NODE;
	write(fd[1],&head,sizeof(head));
	
	//send execute path
	uint16_t pipeLength = strlen(path) + 1;
	write(fd[1],&pipeLength,sizeof(pipeLength));
	write(fd[1],path,pipeLength);

	//send args count
	write(fd[1],&c,sizeof(c));

	//send args
	int i;
	for(i = 0;i < c;i++){
		uint16_t strLen = strlen(args[i]) + 1; 
		write(fd[1],&strLen,sizeof(strLen));
		write(fd[1],args[i],strLen);
	}

	//wait result
	int res = 0;
	read(fd[0],&res,sizeof(res));

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return res;
}


void nodeSystemPrintNodeList(int* argc,char** args){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));

	//send message head
	uint8_t head = PIPE_NODE_LIST;
	write(fd[1],&head,sizeof(head));

	nodeData** itr;
	//print active list
	fprintf(stdout,
					"--------------------active node list--------------------\n");

	uint16_t activeNodeCount;
	read(fd[0],&activeNodeCount,sizeof(activeNodeCount));
	

	int i;
	for(i = 0;i < activeNodeCount;i++){
		char name[PATH_MAX];
		char filePath[PATH_MAX];
		size_t len;

		//print head
		fprintf(stdout,"\n"
					"|------------------------------------------");
		
		//receive node name
		read(fd[0],&len,sizeof(len));
		read(fd[0],name,len);

		//receive node file
		read(fd[0],&len,sizeof(len));
		read(fd[0],filePath,len);

		//print node anme and path
		fprintf(stdout,"\n"
					"|\tname:%s\n"
					"|\tpath:%s\n"
					,name,filePath);
		
		//receive pipe count
		uint16_t pipeCount;
		read(fd[0],&pipeCount,sizeof(pipeCount));

		int j;
		for(j = 0;j < pipeCount;j++){
			nodePipe data;
			char nodeName[PATH_MAX];
			char pipeName[PATH_MAX];

			//receive pipe name
			read(fd[0],&len,sizeof(len));
			read(fd[0],pipeName,len);

			//receive node type
			read(fd[0],&data.type,sizeof(data.type));
			read(fd[0],&data.unit,sizeof(data.unit));
			read(fd[0],&data.length,sizeof(data.length));

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
			read(fd[0],&isConnected,sizeof(isConnected));

			if(isConnected){
				//recive connect node and pipe name
				read(fd[0],&len,sizeof(len));
				read(fd[0],nodeName,len);
				read(fd[0],&len,sizeof(len));
				read(fd[0],pipeName,len);

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

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);
}

int nodeSystemConnect(char* const inNode,char* const inPipe,char* const outNode,char* const outPipe){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_NODE_CONNECT;
	write(fd[1],&head,sizeof(head));
	
	//send in pipe
	size_t len = strlen(inNode)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],inNode,len);
	len = strlen(inPipe)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],inPipe,len);

	//send out pipe
	len = strlen(outNode)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],outNode,len);
	len = strlen(outPipe)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],outPipe,len);

	//wait result
	int res = 0;
	read(fd[0],&res,sizeof(res));

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return res;
}

int nodeSystemDisConnect(char* const inNode,char* const inPipe){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_NODE_DISCONNECT;
	write(fd[1],&head,sizeof(head));
	
	//send  in pipe
	size_t len = strlen(inNode)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],inNode,len);
	len = strlen(inPipe)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],inPipe,len);

	//wait result
	int res = 0;
	read(fd[0],&res,sizeof(res));

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return res;
}

int nodeSystemSetConst(char* const constNode,char* const constPipe,int valueCount,char** setValue){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_NODE_SET_CONST;
	write(fd[1],&head,sizeof(head));
	
	//send const pipe
	size_t len = strlen(constNode)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],constNode,len);
	len = strlen(constPipe)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],constPipe,len);

	//send const len
	write(fd[1],&valueCount,sizeof(valueCount));

	//get result
	int res = 0;
	read(fd[0],&res,sizeof(res));
	if(res != 0)
		return res;

	int i;
	for(i = 0;i < valueCount;i++){
		//send value
		len = strlen(setValue[i])+1;
		write(fd[1],&len,sizeof(len));
		write(fd[1],setValue[i],len);
	}

	//get result
	read(fd[0],&res,sizeof(res));

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return res;
}

int nodeSystemSave(char* const path){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_SAVE;
	write(fd[1],&head,sizeof(head));
	
	//send const pipe
	size_t len = strlen(path)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],path,len);

	//get result
	int res = 0;
	read(fd[0],&res,sizeof(res));

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return res;
}

int nodeSystemLoad(char* const path){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//open laod file
	FILE* loadFile = fopen(path,"r");
	if(loadFile == NULL){
		fprintf(stdout,"Failed fopen(%s) : %s\n",path,strerror(errno));
		return NODE_ERROR_NONE_SUCH_THAT;
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
			fprintf(stdout,"failed load node name of %s\n",nodePath);
			fclose(loadFile);
			return NODE_ERROR_INVALID_ARGS;
		}

		//print name and path
		nodePath[strlen(nodePath)-1] = '\0';
		nodeName[strlen(nodeName)-1] = '\0';
		fprintf(stdout,"load node \nname:%s\npath:%s\n",nodeName,nodePath);
		
		char* args[4] = {nodePath,"-name",nodeName,NULL};
		int code = nodeSystemAddNode(nodePath,args);
		
		if(code  != 0)
			fprintf(stdout,"add node failed:code %d\n",code);
		else
			fprintf(stdout,"add node success\n");
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
			fprintf(stdout,"failed load pipe name of %s\n",nodeName);
			fclose(loadFile);
			return NODE_ERROR_INVALID_ARGS;
		}

		//get connect node name
		char connectNodeName[4096];
		if(fgets(connectNodeName,sizeof(connectNodeName),loadFile) != connectNodeName || connectNodeName[0] == '\n'){
			fprintf(stdout,"failed load connect node name\n");
			fclose(loadFile);
			return NODE_ERROR_INVALID_ARGS;
		}

		//get connect pipe name
		char connectPipeName[4096];
		if(fgets(connectPipeName,sizeof(connectPipeName),loadFile) != connectPipeName || connectPipeName[0] == '\n'){
			fprintf(stdout,"failed load pipe name of %s\n",connectNodeName);
			fclose(loadFile);
			return NODE_ERROR_INVALID_ARGS;
		}

		//print name and path
		nodeName[strlen(nodeName)-1] = '\0';
		pipeName[strlen(pipeName)-1] = '\0';
		connectNodeName[strlen(connectNodeName)-1] = '\0';
		connectPipeName[strlen(connectPipeName)-1] = '\0';
		fprintf(stdout,"load pipe connection \ninNode:%s\ninPipe:%s\noutNode:%s\noutPipe:%s\n",nodeName,pipeName,connectNodeName,connectPipeName);
		
		int code = nodeSystemConnect(nodeName,pipeName,connectNodeName,connectPipeName);
		
		if(code  != 0)
			fprintf(stdout,"connect node failed:code %d\n",code);
		else
			fprintf(stdout,"connect node success\n");
	};

	//set const
	while(1){
		//get node path
		char nodeName[4096];
		if(fgets(nodeName,sizeof(nodeName),loadFile) != nodeName || nodeName[0] == '\n'){
			break;
		}

		//get node name
		char pipeName[4096];
		if(fgets(pipeName,sizeof(pipeName),loadFile) != pipeName || pipeName[0] == '\n'){
			fprintf(stdout,"failed load pipe name of %s\n",nodeName);
			fclose(loadFile);
			return NODE_ERROR_INVALID_ARGS;
		}

		//get data length
		char dataLength[4096];
		if(fgets(dataLength,sizeof(dataLength),loadFile) != dataLength || dataLength[0] == '\n'){
			fprintf(stdout,"failed load data length\n");
			fclose(loadFile);
			return NODE_ERROR_INVALID_ARGS;
		}

		int size;
		sscanf(dataLength,"%d",&size);

		//get connect pipe name
		void* mem = malloc(size);
		fread(mem,size,1,loadFile);


		//print name and path
		nodeName[strlen(nodeName)-1] = '\0';
		pipeName[strlen(pipeName)-1] = '\0';

		fprintf(stdout,"load const pipe \nNode:%s\nPipe:%s\n",nodeName,pipeName);
	
		//send message head
		uint8_t head = PIPE_LOAD;
		write(fd[1],&head,sizeof(head));

		//send node name and piepe name
		size_t len = strlen(nodeName)+1;
		write(fd[1],&len,sizeof(len));
		write(fd[1],nodeName,len);
		len = strlen(pipeName)+1;
		write(fd[1],&len,sizeof(len));
		write(fd[1],pipeName,len);

		//send data
		write(fd[1],&size,sizeof(size));
		write(fd[1],mem,size);

		//get result
		int code;
		while(read(fd[0],&code,sizeof(code)) != sizeof(code));

		free(mem);

		if(code  != 0)
			fprintf(stdout,"set const failed:code %d\n",code);
		else
			fprintf(stdout,"set const success\n");
	};

	fclose(loadFile);

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return 0;
}

char** nodeSystemGetConst(char* const constNode,char* const constPipe,int* retCode){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_NODE_GET_CONST;
	write(fd[1],&head,sizeof(head));
	
	//send const pipe
	size_t len = strlen(constNode)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],constNode,len);
	len = strlen(constPipe)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],constPipe,len);

	//receive count
	read(fd[0],retCode,sizeof(*retCode));
	if(retCode < 0)
		return NULL;

	//malloc mem
	char** values = malloc(sizeof(char*) * *retCode);

	int i;
	for(i = 0;i < *retCode;i++){
		//receive value
		read(fd[0],&len,sizeof(len));
		values[i] = malloc(len);
		read(fd[0],values[i],len);
	}

	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	return values;
}

void nodeSystemTimerRun(){
	//send message head
	uint8_t head = PIPE_TIMER_RUN;
	write(fd[1],&head,sizeof(head));
}

void nodeSystemTimerStop(){
	//send message head
	uint8_t head = PIPE_TIMER_STOP;
	write(fd[1],&head,sizeof(head));
}

char** nodeSystemGetNodeNameList(int* counts){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_GET_NODE_NAME_LIST;
	write(fd[1],&head,sizeof(head));
	
	//receive node count
	uint16_t nodeCounts;
	read(fd[0],&nodeCounts,sizeof(nodeCounts));
	
	//malloc name list
	char** names = malloc(sizeof(char*) * nodeCounts);

	int i;
	for(i = 0;i < nodeCounts;i++){
		size_t len;
		read(fd[0],&len,sizeof(len));
		names[i] = malloc(len);
		read(fd[0],names[i],len);
	}


	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	counts[0] = nodeCounts;
	return names;
}

char** nodeSystemGetPipeNameList(char* nodeName,int* counts){
	//set blocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));
	
	//send message head
	uint8_t head = PIPE_GET_PIPE_NAME_LIST;
	write(fd[1],&head,sizeof(head));

	//send node name
	size_t len = strlen(nodeName)+1;
	write(fd[1],&len,sizeof(len));
	write(fd[1],nodeName,len);
	
	//receive pipe count
	uint16_t pipeCounts;
	read(fd[0],&pipeCounts,sizeof(pipeCounts));
	
	//malloc name list
	char** names = malloc(sizeof(char*) * pipeCounts);

	int i;
	for(i = 0;i < pipeCounts;i++){
		read(fd[0],&len,sizeof(len));
		names[i] = malloc(len);
		read(fd[0],names[i],len);
	}


	//set nonblocking
	fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

	counts[0] = pipeCounts;
	return names;
}

static void nodeSystemLoop(){
	uint8_t head;
	//check inactive nodes
	nodeData** itr;
	LINEAR_LIST_FOREACH(inactiveNodeList,itr){
		int ret = nodeBegin(*itr);
		if(ret == 0){
			nodeData* data = *itr;
			LINEAR_LIST_ERASE(itr);

			LINEAR_LIST_PUSH(activeNodeList,data);
		}else if(ret < 0){
			//kill
			kill((*itr)->pid,SIGTERM);

			//deleate node
			nodeDeleate(*itr);

			//deleate from list
			LINEAR_LIST_ERASE(itr);
		}
	}

	//check active node
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(kill((*itr)->pid,0)){
			//process is terminated

			//remove mem
			int i;
			for(i = 0;(*itr)->pipeCount;i++){
				if((*itr)->pipes[i].type != NODE_PIPE_IN){
					shmctl((*itr)->pipes[i].sID,IPC_RMID,NULL);
					if(systemSettingMemory->isNoLog)
						fprintf(logFile,"%s:[%s]shmctl(IPC_RMID):%s\n",getRealTimeStr(),(*itr)->name,strerror(errno));
				}
			}

			//deleate node
			nodeDeleate(*itr);
			//deleate from list
			LINEAR_LIST_ERASE(itr);
		}
	}

	//message from parent 
	if(read(fd[0],&head,sizeof(head)) == 1){
		//set blocking
		fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) & (~O_NONBLOCK));

		//recive from parent
		switch(head){
			case PIPE_ADD_NODE:{//add node		
				pipeAddNode();	
				break;
			}
			case PIPE_NODE_LIST:{//node list
				pipeNodeList();		
				break;
			}
			case PIPE_NODE_CONNECT:{//node list
				pipeNodeConnect();		
				break;
			}
			case PIPE_NODE_DISCONNECT:{
				pipeNodeDisConnect();
				break;
			}
			case PIPE_GET_NODE_NAME_LIST:{
				pipeNodeGetNodeNameList();
				break;
			}
			case PIPE_GET_PIPE_NAME_LIST:{
				pipeNodeGetPipeNameList();
				break;
			}
			case PIPE_NODE_SET_CONST:{
				pipeNodeSetConst();
				break;
			}
			case PIPE_NODE_GET_CONST:{
				pipeNodeGetConst();
				break;
			}
			case PIPE_SAVE:{
				pipeSave();
				break;
			}
			case PIPE_LOAD:{
				pipeLoad();
				break;
			}
			case PIPE_TIMER_RUN:{
				pipeTimerRun();
				break;
			}
			case PIPE_TIMER_STOP:{
				pipeTimerStop();
				break;
			}
			default:
		}

		//set nonblocking
		fcntl(fd[0] ,F_SETFL,fcntl(fd[0] ,F_GETFL) | O_NONBLOCK);

		if(systemSettingMemory->isNoLog)
			fflush(logFile);
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
	if(pipe(pipeTx) < 0 || pipe(pipeRx) < 0 || pipe(pipeErr) < 0)
		return -1;

	//fork
	int process = fork();
	if(process == -1)
		return -1;

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
	
	int ret = fileReadWithTimeOut(node->fd[0],&header_buffer,sizeof(uint32_t),1);

	if(ret == 0){
		fprintf(logFile,"%s:[%s]Rloop\n",getRealTimeStr(),node->name);
		return 1;
	}
	else if((ret != sizeof(uint32_t))){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:[%s]Recive header sequence timeout\n",getRealTimeStr(),node->name);
		return -1;
	}else if(header_buffer != _node_begin_head){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:[%s]Failed recive header\n",getRealTimeStr(),node->name);
		return -2;
	}

	//give pipe
	int i;
	for(i = 0;i < node->pipeCount;i++){
		if(node->pipes[i].type != NODE_PIPE_IN){
			size_t memSize = NODE_DATA_UNIT_SIZE[node->pipes[i].unit] * node->pipes[i].length + 1;

			int sId = shmget(IPC_PRIVATE, memSize,0666);
			if(sId < 0){
				if(systemSettingMemory->isNoLog)
					fprintf(logFile,"%s:[%s.%s]failed shmget() : %s\n",getRealTimeStr(),node->name,node->pipes[i].pipeName,strerror(errno));
				return -1;
			}

			
			write(node->fd[1],&sId,sizeof(sId));
			node->pipes[i].sID = sId;
		}
	}

	if((fileReadWithTimeOut(node->fd[0],&header_buffer,sizeof(uint32_t),1000) != sizeof(uint32_t))){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:[%s]Recive eof sequence timeout\n",getRealTimeStr(),node->name);
		return -1;
	}else if(header_buffer != _node_begin_eof){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:[%s]failed recive eof\n",getRealTimeStr(),node->name);
		return -2;
	}

	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:[%s]node is activate\n",getRealTimeStr(),node->name);
	return 0;
}

static void nodeDeleate(nodeData* node){

	//log
	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:[%s]node is deleate\n",getRealTimeStr(),node->name);

	//releace mem
	int i;
	for(i = 0;i < node->pipeCount;i++){
		free(node->pipes[i].pipeName);
	}
	
	free(node->pipes);
	free(node->name); 
	free(node->filePath); 
	free(node);

}

static int sendNodeEnv(int fd,nodeData* data){
	
}


static int receiveNodeProperties(int fd,nodeData* node){
	char recvBuffer[1024];
	
	//receive header
	if(fileReadWithTimeOut(fd,recvBuffer,sizeof(_node_init_head),1000) != sizeof(_node_init_head)){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:Header receive sequence time out\n",getRealTimeStr());
		return -1;
	}else if(((uint32_t*)recvBuffer)[0] != _node_init_head){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:Received header is invalid\n",getRealTimeStr());
		return -2;
	}
	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:Header receive succsee\n",getRealTimeStr());

	//read system Env
	fileWrite(fd,&systemSettingKey.shmId,sizeof(int));
	fileWrite(fd,&systemSettingKey.semId,sizeof(int));

	//send path
	char path[PATH_MAX];
	sprintf(path,"%s/%s.txt",logFolder,node->name);
	fileWriteStr(fd,path);

	//receive pipe count
	if(fileReadWithTimeOut(fd,recvBuffer,sizeof(uint16_t),1000) != sizeof(uint16_t)){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:Pipe count receive sequence time out\n",getRealTimeStr());
		return -1;
	}	
	node->pipeCount = ((uint16_t*)recvBuffer)[0];
	node->pipes = malloc(sizeof(nodePipe)*node->pipeCount);
	memset(node->pipes,0,sizeof(nodePipe)*node->pipeCount);
	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:Pipe count %d\n",getRealTimeStr(),(int)node->pipeCount);
	
	//receive pipe
	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:Pipe receive sequence begin\n",getRealTimeStr());
	uint16_t i;
	for(i = 0;i < node->pipeCount;i++){
		//type
		if(fileReadWithTimeOut(fd,&node->pipes[i].type,sizeof(uint8_t),1000) != sizeof(uint8_t)){
			if(systemSettingMemory->isNoLog)
				fprintf(logFile,"%s:Pipe type receive sequence time out\n",getRealTimeStr());
			return -1;
		}	

		//unit
		if(fileReadWithTimeOut(fd,&node->pipes[i].unit,sizeof(uint8_t),1000) != sizeof(uint8_t)){
			if(systemSettingMemory->isNoLog)
				fprintf(logFile,"%s:Pipe unit receive sequence time out\n",getRealTimeStr());
			return -1;
		}	
		
		//length
		if(fileReadWithTimeOut(fd,&node->pipes[i].length,sizeof(uint16_t),1000) != sizeof(uint16_t)){
			if(systemSettingMemory->isNoLog)
				fprintf(logFile,"%s:Unit length receive sequence time out\n",getRealTimeStr());
			return -1;
		}	
		
		//name
		uint32_t len = fileReadStrWithTimeOut(fd,recvBuffer,sizeof(recvBuffer),1000);
		if(len == 0 || recvBuffer[len - 1] != '\0'){
			if(systemSettingMemory->isNoLog)
				fprintf(logFile,"%s:Unit length receive sequence time out\n",getRealTimeStr());
			return -1;
		}
		node->pipes[i].pipeName = malloc(len);
		strcpy(node->pipes[i].pipeName,recvBuffer);
	
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,	
				"--------------------------------------\n"
				"PipeName: %s\n"
				"PipeType: %s\n"
				"PipeUnit: %s\n"
				"ArraySize: %d\n"
				"--------------------------------------\n",
				node->pipes[i].pipeName,
				NODE_PIPE_TYPE_STR[node->pipes[i].type],
				NODE_DATA_UNIT_STR[node->pipes[i].unit],
				node->pipes[i].length);
	}
	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:Pipe receive sequence end\n",getRealTimeStr());

	//recive eof
	if(fileReadWithTimeOut(fd,recvBuffer,sizeof(_node_init_eof),1000) != sizeof(_node_init_eof)){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:EOF receive sequence time out\n",getRealTimeStr());
		return -1;
	}else if(((uint32_t*)recvBuffer)[0] != _node_init_eof){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:received EOF is invalid\n",getRealTimeStr());
		return -2;
	}
	if(systemSettingMemory->isNoLog)
		fprintf(logFile,"%s:Header EOF succsee\n",getRealTimeStr());

	return 0;
}

static void pipeAddNode(){
	//init struct
	nodeData* data = malloc(sizeof(nodeData));
	memset(data,0,sizeof(nodeData));

	//get path
	uint16_t pathLength;
	fileRead(fd[0],&pathLength,sizeof(pathLength));
	data->filePath = malloc(pathLength);
	fileRead(fd[0],data->filePath,pathLength);
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
		uint16_t len;
		fileRead(fd[0],&len,sizeof(len));
		args[i] = malloc(len);

		fileRead(fd[0],args[i],len);
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
		int res = NODE_ERROR_ALREADY;
		write(fd[1],&res,sizeof(res));
		return;
	}

	

	//execute program
	data->pid = popenRWasNonBlock(data->filePath,data->fd);
	if(data->pid < 0){
		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:[%s]%s\n",getRealTimeStr(),data->name,strerror(errno));
		free(data);

		int res = NODE_ERROR_FOPEN;
		write(fd[1],&res,sizeof(res));
		return;
	}

	
	//load properties
	if(receiveNodeProperties(data->fd[0],data)){
		kill(data->pid,SIGTERM);
		free(data);

		int res = NODE_ERROR_NONE_SUCH_THAT;
		write(fd[1],&res,sizeof(res));
		return;
	}
	else
		LINEAR_LIST_PUSH(inactiveNodeList,data);


	int res = NODE_SUCCESS;
	write(fd[1],&res,sizeof(res));	
}

static void pipeNodeList(){
	uint16_t nodeCount = 0;
	nodeData** itr;
	
	//get node count
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		nodeCount++;
	}

	//send node count
	write(fd[1],&nodeCount,sizeof(nodeCount));
	
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		//send node name
		size_t len = strlen((*itr)->name)+1;
		write(fd[1],&len,sizeof(len));
		write(fd[1],(*itr)->name,len);
		
		//send file path
		len = strlen((*itr)->filePath)+1;
		write(fd[1],&len,sizeof(len));
		write(fd[1],(*itr)->filePath,len);

		//send pipe count
		write(fd[1],&(*itr)->pipeCount,sizeof((*itr)->pipeCount));
		
		int i;
		for(i = 0;i < (*itr)->pipeCount;i++){
			//semd pipe option
			len = strlen((*itr)->pipes[i].pipeName)+1;
			write(fd[1],&len,sizeof(len));
			write(fd[1],(*itr)->pipes[i].pipeName,len);
			write(fd[1],&(*itr)->pipes[i].type,sizeof((*itr)->pipes[i].type));
			write(fd[1],&(*itr)->pipes[i].unit,sizeof((*itr)->pipes[i].unit));
			write(fd[1],&(*itr)->pipes[i].length,sizeof((*itr)->pipes[i].length));

			//send state
			uint8_t isConnected = (*itr)->pipes[i].connectPipe != NULL;
			write(fd[1],&isConnected,sizeof(isConnected));

			if(isConnected){
				//send connect node and pipe
				len = strlen((*itr)->pipes[i].connectNode)+1;
				write(fd[1],&len,sizeof(len));
				write(fd[1],(*itr)->pipes[i].connectNode,len);
				len = strlen((*itr)->pipes[i].connectPipe)+1;
				write(fd[1],&len,sizeof(len));
				write(fd[1],(*itr)->pipes[i].connectPipe,len);

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
	size_t len;
	read(fd[0],&len,sizeof(len));
	read(fd[0],inNode,len);
	read(fd[0],&len,sizeof(len));
	read(fd[0],inPipe,len);

	//receive out pipe
	read(fd[0],&len,sizeof(len));
	read(fd[0],outNode,len);
	read(fd[0],&len,sizeof(len));
	read(fd[0],outPipe,len);

	//finde pipe
	nodePipe* in = NULL,*out = NULL;
	nodeData *node_in = NULL,*node_out = NULL;
	uint16_t pipe_in = 0;
	int outputMem = -1;

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
					outputMem = (*itr)->pipes[i].sID;
					out = &(*itr)->pipes[i];
					break;
				}
			}
		}
	}

	int res = 0;
	if(in == NULL || out == NULL){
		res = NODE_ERROR_NONE_SUCH_THAT;
	}else if(in->type != NODE_PIPE_IN || out->type != NODE_PIPE_OUT || in->unit != out->unit || in->length != out->length){
		res = NODE_ERROR_INVALID_ARGS;
	}else{
		write(node_in->fd[1],&pipe_in,sizeof(pipe_in));
		write(node_in->fd[1],&outputMem,sizeof(outputMem));
		in->connectNode = node_out->name;
		in->connectPipe = out->pipeName;

		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:connect %s %s to %s %s\n",getRealTimeStr(),inNode,inPipe,outNode,outPipe);
	}

	//send result
	write(fd[1],&res,sizeof(res));
}

static void pipeNodeDisConnect(){
	char inNode[PATH_MAX];
	char inPipe[PATH_MAX];
	
	//receive in pipe
	size_t len;
	read(fd[0],&len,sizeof(len));
	read(fd[0],inNode,len);
	read(fd[0],&len,sizeof(len));
	read(fd[0],inPipe,len);

	//finde pipe
	nodePipe* in = NULL;
	nodeData *node_in = NULL;
	uint16_t pipe_in = 0;
	int outputMem = 0;

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
		res = NODE_ERROR_NONE_SUCH_THAT;
	}else if(in->type == NODE_PIPE_OUT){
		res = NODE_ERROR_INVALID_ARGS;
	}else{
		write(node_in->fd[1],&pipe_in,sizeof(pipe_in));
		write(node_in->fd[1],&outputMem,sizeof(outputMem));
		in->connectNode = NULL;
		in->connectPipe = NULL;

		if(systemSettingMemory->isNoLog)
			fprintf(logFile,"%s:disconnect %s %s\n",getRealTimeStr(),inNode,inPipe);
	}

	//send result
	write(fd[1],&res,sizeof(res));
}

static void pipeNodeSetConst(){
	char constNode[PATH_MAX];
	char constPipe[PATH_MAX];
	
	//receive in pipe
	size_t len;
	read(fd[0],&len,sizeof(len));
	read(fd[0],constNode,len);
	read(fd[0],&len,sizeof(len));
	read(fd[0],constPipe,len);

	//recive value count
	int count;
	read(fd[0],&count,sizeof(count));

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
		res = NODE_ERROR_NONE_SUCH_THAT;
	}else if(pipe_const->type != NODE_PIPE_CONST || pipe_const->length != count){
		res = NODE_ERROR_INVALID_ARGS;
	}

	//send result
	write(fd[1],&res,sizeof(res));

	if(res == 0){
		uint16_t size = NODE_DATA_UNIT_SIZE[pipe_const->unit];
		void* tmpBuffer = malloc(size*pipe_const->length);
		int flag = 1;

		if(tmpBuffer == NULL){
			res = NODE_ERROR_MEMORY;
			write(fd[1],&res,sizeof(res));
		}

		//read data
		switch(pipe_const->unit){
			case NODE_UNIT_CHAR:{
				int i;
				for(i = 0;i < count;i++){
					read(fd[0],&len,sizeof(len));
					char* str = malloc(len);
					read(fd[0],str,len);
					flag &= sscanf(str,"%c",&((char*)tmpBuffer)[i]);
					free(str);
				}
			}
			break;
			case NODE_UNIT_BOOL:{
				int i;
				for(i = 0;i < count;i++){
					read(fd[0],&len,sizeof(len));
					char* str = malloc(len);
					read(fd[0],str,len);
					int isTrue;
					flag &= sscanf(str,"%d",&isTrue);
					((uint8_t*)tmpBuffer)[i] = (isTrue != 0);
					free(str);
				}
			}
			break;
			case NODE_UNIT_INT8:
			case NODE_UNIT_INT16:
			case NODE_UNIT_INT32:
			case NODE_UNIT_INT64:{
				int i;
				for(i = 0;i < count;i++){
					read(fd[0],&len,sizeof(len));
					char* str = malloc(len);
					read(fd[0],str,len);
					long value;
					flag &= sscanf(str,"%ld",&value);
					memcpy(tmpBuffer+i*size,&value,size);
					if(value < 0 && (((-1l)<<size*8)&~value))
						flag = 0;
					else if(value > 0 && ((-1l)<<size*8)&value)
						flag = 0;
					free(str);
				}
			}
			break;
			case NODE_UNIT_UINT8:
			case NODE_UNIT_UINT16:
			case NODE_UNIT_UINT32:
			case NODE_UNIT_UINT64:{
				int i;
				for(i = 0;i < count;i++){
					read(fd[0],&len,sizeof(len));
					char* str = malloc(len);
					read(fd[0],str,len);
					unsigned long value;
					flag &= sscanf(str,"%lu",&value);
					memcpy(tmpBuffer+i*size,&value,size);
					if(((-1l)<<size*8)&value)
						flag = 0;
					free(str);
				}
			}
			break;
			case NODE_UNIT_FLOAT:{
				int i;
				for(i = 0;i < count;i++){
					read(fd[0],&len,sizeof(len));
					char* str = malloc(len);
					read(fd[0],str,len);
					flag &= sscanf(str,"%f",&((float*)tmpBuffer)[i]);
					free(str);
				}
			}
			break;
			case NODE_UNIT_DOUBLE:{
				int i;
				for(i = 0;i < count;i++){
					read(fd[0],&len,sizeof(len));
					char* str = malloc(len);
					read(fd[0],str,len);
					flag &= sscanf(str,"%lf",&((double*)tmpBuffer)[i]);
					free(str);
				}
			}
			break;
			
		}

		res = 0;
		if(flag == 0){
			res = NODE_ERROR_INVALID_ARGS;
		}else{
			//cpy data
			uint8_t* memory = shmat(pipe_const->sID,NULL,0);
			if(memory  > 0){
				memory[0]++;
				memcpy(memory+1,tmpBuffer,size*pipe_const->length);
				shmdt(memory);
			}
			else{
				res = NODE_ERROR_MEMORY;
			}
		}

		//free
		free(tmpBuffer);

		//send result
		write(fd[1],&res,sizeof(res));
	}
}

static void pipeNodeGetConst(){
	char constNode[PATH_MAX];
	char constPipe[PATH_MAX];
	
	//receive in pipe
	size_t len;
	read(fd[0],&len,sizeof(len));
	read(fd[0],constNode,len);
	read(fd[0],&len,sizeof(len));
	read(fd[0],constPipe,len);

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
	void* memory;
	if(pipe_const == NULL){
		res = NODE_ERROR_NONE_SUCH_THAT;
	}else if(pipe_const->type != NODE_PIPE_CONST){
		res = NODE_ERROR_INVALID_ARGS;
	}else if((memory = shmat(pipe_const->sID,NULL,0)) < 0){
		res = NODE_ERROR_MEMORY;
	}else{			
		res = pipe_const->length;
	}

	//send result
	write(fd[1],&res,sizeof(res));


	if(res > 0){
		char value[1024];
		uint16_t size = NODE_DATA_UNIT_SIZE[pipe_const->unit];

		//read data
		switch(pipe_const->unit){
			case NODE_UNIT_CHAR:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%c",((char*)memory)[i+1]);

					len = strlen(value)+1;
					write(fd[1],&len,sizeof(len));
					write(fd[1],value,len);
				}
			}
			break;
			case NODE_UNIT_BOOL:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%d",(int)((char*)memory)[i+1]);

					len = strlen(value)+1;
					write(fd[1],&len,sizeof(len));
					write(fd[1],value,len);
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

					len = strlen(value)+1;
					write(fd[1],&len,sizeof(len));
					write(fd[1],value,len);
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

					len = strlen(value)+1;
					write(fd[1],&len,sizeof(len));
					write(fd[1],value,len);
				}
			}
			break;
			case NODE_UNIT_FLOAT:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%f",((float*)(memory + 1))[i]);

					len = strlen(value)+1;
					write(fd[1],&len,sizeof(len));
					write(fd[1],value,len);
				}
			}
			break;
			case NODE_UNIT_DOUBLE:{
				int i;
				for(i = 0;i < pipe_const->length;i++){
					sprintf(value,"%lf",((double*)(memory + 1))[i]);

					len = strlen(value)+1;
					write(fd[1],&len,sizeof(len));
					write(fd[1],value,len);
				}
			}
			break;	
		}

		shmdt(memory);
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
	read(fd[0],&len,sizeof(len));
	read(fd[0],saveFilePath,len);

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
					if((mem = shmat((*itr)->pipes[i].sID,NULL,SHM_RDONLY)) > 0){
						fprintf(saveFile,"%d\n",(*itr)->pipes[i].length*NODE_DATA_UNIT_SIZE[(*itr)->pipes[i].unit]);
						fwrite(mem+1,NODE_DATA_UNIT_SIZE[(*itr)->pipes[i].unit],(*itr)->pipes[i].length,saveFile);
						shmdt(mem);
					}else{
						res = NODE_ERROR_MEMORY;
						if(!systemSettingMemory->isNoLog)
							fprintf(logFile,"%s:failed shmat(%s of %s) %s\n",getRealTimeStr(),(*itr)->pipes[i].pipeName,(*itr)->name,strerror(errno));
					}
				}
			}
		}
		
		//insert space
		fprintf(saveFile,"\n");
		fclose(saveFile);
	}else{
		res = NODE_ERROR_NONE_SUCH_THAT;
	}


	//send result
	write(fd[1],&res,sizeof(res));
}

static void pipeLoad(){
	char nodeName[PATH_MAX];
	char pipeName[PATH_MAX];
	int size;

	//recive node name pipe name
	size_t len;
	read(fd[0],&len,sizeof(len));
	read(fd[0],nodeName,len);
	read(fd[0],&len,sizeof(len));
	read(fd[0],pipeName,len);

	fprintf(logFile,"load const pipe \nNode:%s\nPipe:%s\n",nodeName,pipeName);
	//receive data
	read(fd[0],&size,sizeof(size));
	void* mem = malloc(size);
	read(fd[0],mem,size);

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
		res = NODE_ERROR_NONE_SUCH_THAT;
	}else{
		//check size
		if(size > pipe_const->length)
			size = pipe_const->length;
		
		void* constMem;
		if((constMem = shmat(pipe_const->sID,NULL,0)) <= 0){
			res = NODE_ERROR_MEMORY;
		}else{
			((uint8_t*)constMem)[0]++;
			memcpy(constMem+1,mem,size);
			shmdt(constMem);
		}
	}

	//free
	free(mem);

	//send result
	write(fd[1],&res,sizeof(res));
}

static void pipeTimerRun(){
	isTimerRunning = 1;
}

static void pipeTimerStop(){
	isTimerRunning = 0;
}

static void pipeNodeGetNodeNameList(){
	uint16_t nodeCount = 0;
	nodeData** itr;
	
	//get node count
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		nodeCount++;
	}

	//send node count
	write(fd[1],&nodeCount,sizeof(nodeCount));
	
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		//send node name
		size_t len = strlen((*itr)->name)+1;
		write(fd[1],&len,sizeof(len));
		write(fd[1],(*itr)->name,len);
	}
}

static void pipeNodeGetPipeNameList(){
	uint16_t nodeCount = 0;
	nodeData** itr;
	char nodeName[PATH_MAX];
	size_t len;

	//get node name
	read(fd[0],&len,sizeof(len));
	read(fd[0],nodeName,len);
	
	//get node count
	LINEAR_LIST_FOREACH(activeNodeList,itr){
		if(strcmp((*itr)->name,nodeName) == 0){
			//send pipe count
			write(fd[1],&(*itr)->pipeCount,sizeof((*itr)->pipeCount));

			int i;
			for(i = 0;i < (*itr)->pipeCount;i++){
				//send pipe name
				len = strlen((*itr)->pipes[i].pipeName)+1;
				write(fd[1],&len,sizeof(len));
				write(fd[1],(*itr)->pipes[i].pipeName,len);
			}

			return;
		}
	}


	//if node is not found
	uint16_t zero = 0;
	write(fd[1],&zero,sizeof(zero));
}

#else

typedef struct{
	int shmID;
	int semID;
	void* memory;
	char* pipeName;
	uint8_t count;
	uint8_t type;
	uint8_t unit;
	uint16_t length;
} _node_pipe;

static uint8_t _nodeSystemIsActive = 0;
static NODE_DEBUG_MODE _dMode = NODE_DEBUG_MESSAGE;

static FILE* logFile;
static uint16_t _pipe_count = 0;
static _node_pipe* _pipes = NULL;
static int _self;
static int _parent;

int nodeSystemInit(){
	//Check system state
	if(_nodeSystemIsActive){
		return NODE_ERROR_BAD_STATUS;
	}
	

	//Set Pid
	_self = getpid();
	_parent = getppid();

	//send header
	write(STDOUT_FILENO,&_node_init_head,sizeof(_node_init_head));

	//read system Env
	fileRead(STDIN_FILENO,&systemSettingKey.shmId,sizeof(int));
	fileRead(STDIN_FILENO,&systemSettingKey.semId,sizeof(int));
	systemSettingKey.shmMap = shmat(systemSettingKey.shmId,NULL,SHM_RDONLY);

	if(systemSettingKey.shmMap < 0)
		return NODE_ERROR_MEMORY;

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
			return NODE_ERROR_FOPEN ;
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

	return NODE_SUCCESS;
}

int nodeSystemAddPipe(char* const pipeName,NODE_PIPE_TYPE type,NODE_DATA_UNIT unit,uint16_t arrayLength,const void* buff){
	//check system state
	if(_nodeSystemIsActive){
		return NODE_ERROR_BAD_STATUS;
	}

	//check pipe count
	if(_pipe_count == 0xFFFF){
		return NODE_ERROR_LIMIT;
	}
	
	//name check
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		if(strcmp(_pipes[i].pipeName,pipeName) == 0)
			return NODE_ERROR_ALREADY;
	}

	//malloc piep struct
	_node_pipe pipe = {};
	
	//cpy data
	pipe.type = type;
	pipe.unit = unit;
	pipe.length = arrayLength;
	pipe.pipeName = malloc(strlen(pipeName)+1);
	if(!pipe.pipeName){
		return NODE_ERROR_MEMORY;
	}
	strcpy(pipe.pipeName,pipeName);

	//add pipe array
	_node_pipe* tmp = _pipes;
	_pipes = realloc(_pipes,sizeof(_node_pipe)*(_pipe_count+1));
	if(!_pipes){
		_pipes = tmp;
		free(pipe.pipeName);
		return NODE_ERROR_MEMORY;
	}
	_pipes[_pipe_count] = pipe;

	//Set init value
	if(type == NODE_PIPE_CONST && buff){
		_pipes[_pipe_count].memory = malloc(NODE_DATA_UNIT_SIZE[unit]*arrayLength);
		memcpy(_pipes[_pipe_count].memory,buff,NODE_DATA_UNIT_SIZE[unit]*arrayLength);
	}

	return _pipe_count++;
}

int nodeSystemBegine(){
	//check system state
	if(_nodeSystemIsActive != 1){
		return NODE_ERROR_BAD_STATUS;
	}

	//send header
	fileWrite(STDOUT_FILENO,&_node_begin_head,sizeof(_node_begin_head));

	//receive pipe data
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		//if pipe type is not PIPE_IN
		if(_pipes[i].type != NODE_PIPE_IN){
			//receive share memory
			fileRead(STDIN_FILENO,&_pipes[i].shmID,sizeof(_pipes[i].shmID));
			fileRead(STDIN_FILENO,&_pipes[i].semID,sizeof(_pipes[i].semID));
			
			//if pipe type is PIPE_CONST
			if(_pipes[i].type == NODE_PIPE_CONST){
				//attach share memory for write
				void* initVal = _pipes[i].memory;
				_pipes[i].memory = shmat(_pipes[i].shmID,NULL,0);

				//if attach success
				if(_pipes[i].memory > 0){
					//if initVal is not null
					if(initVal){
						//cpy init value
						memcpy(_pipes[i].memory+1,initVal,
							NODE_DATA_UNIT_SIZE[_pipes[i].unit]*_pipes[i].length);
						//increment write counter
						((uint8_t*)_pipes[i].memory)[0]++;
						//free
						free(initVal);
					}
					//dettach and attach sheare memory for read 
					shmdt(_pipes[i].memory);
					_pipes[i].memory = shmat(_pipes[i].shmID,NULL,SHM_RDONLY);
				}
			}
			else{
				//attach share memory for write
				_pipes[i].memory = shmat(_pipes[i].shmID,NULL,0);
			}

			//if failed shmat
			if(_pipes[i].memory < 0){
				return NODE_ERROR_MEMORY;
			}
		}
	}
	
	//send eof
	fileWrite(STDOUT_FILENO,&_node_begin_eof,sizeof(_node_begin_eof));

	//set nonblocking
	fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) | O_NONBLOCK);

	//set state
	_nodeSystemIsActive = 2;

	return NODE_SUCCESS
}


int nodeSystemLoop(){
	uint16_t  pipeId;
	
	//if 
	if(fileRead(STDIN_FILENO,&pipeId,sizeof(pipeId)) == sizeof(uint16_t)){
		//set blocking
		fcntl(STDIN_FILENO ,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) & (~O_NONBLOCK));

		if(_pipes[pipeId].shmID != 0){
			if(shmdt(_pipes[pipeId].memory) < 0){
				char msg[4096];
				sprintf(msg,"Pipe[%s] failed shmdt():%s",_pipes[pipeId].pipeName,strerror(errno));
				nodeSystemDebugLog(msg);
			}
		}

		_pipes[pipeId].count = 0;
		fileRead(STDIN_FILENO,&_pipes[pipeId].shmID,sizeof(_pipes[pipeId].shmID));
		fileRead(STDIN_FILENO,&_pipes[pipeId].semID,sizeof(_pipes[pipeId].semID));
		if(_pipes[pipeId].shmID != 0){			
			_pipes[pipeId].memory = shmat(_pipes[pipeId].shmID,NULL,SHM_RDONLY);

			char msg[4096];
			sprintf(msg,"Pipe[%s] pipe connected",_pipes[pipeId].pipeName);
			nodeSystemDebugLog(msg);
		}else{
			char msg[4096];
			sprintf(msg,"Pipe[%s] pipe diss connect",_pipes[pipeId].pipeName);
			nodeSystemDebugLog(msg);
		}
		

		//set nonblocking
		fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) | O_NONBLOCK);
	}


	return kill(_parent,0);
}

void nodeSystemDebugLog(char* const str){
	//check system state
	if(_nodeSystemIsActive != 1){
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
		return NODE_ERROR_BAD_STATUS;
	}

	//change mode
	_dMode = mode;

	return NODE_SUCCESS
}

int nodeSystemRead(int pipeID,void* buffer){
	//check system state
	if(_nodeSystemIsActive != 2){
		return NODE_ERROR_BAD_STATUS;
	}
	
	//check pipe type
	if(!_pipes[pipeID].sID || _pipes[pipeID].type == NODE_PIPE_OUT)
		return NODE_ERROR_INVALID_ARGS;
	
	//read count
	uint8_t count = ((char*)_pipes[pipeID].memory)[0];

	//copy data
	memcpy(buffer,_pipes[pipeID].memory+1,NODE_DATA_UNIT_SIZE[_pipes[pipeID].unit] * _pipes[pipeID].length);
	
	if(count == _pipes[pipeID].count)
			return 0;

	_pipes[pipeID].count = count;
	return 1;
}

int nodeSystemWrite(int pipeID,void* const buffer){
	//check system state
	if(_nodeSystemIsActive != 2){
		return NODE_ERROR_BAD_STATUS;
	}
	
	//check pipe type
	if(_pipes[pipeID].type != NODE_PIPE_OUT)
		return NODE_ERROR_INVALID_ARGS;

	//write count
	((uint8_t*)_pipes[pipeID].memory)[0] = ++_pipes[pipeID].count;

	//copy data
	memcpy(_pipes[pipeID].memory+1,buffer,NODE_DATA_UNIT_SIZE[_pipes[pipeID].unit] * _pipes[pipeID].length);

	return 0;
}

int nodeSystemWait(){
	//check system state
	if(_nodeSystemIsActive != 2){
		return NODE_ERROR_BAD_STATUS;
	}

	int pid = fork();	

	if(pid == -1){
		//failed  fork
		char errorMsg[1024];
		sprintf(errorMsg,"%s:%s",__func__,strerror(errno));
		nodeSystemDebugLog(errorMsg);
		return NODE_ERROR_FORK;
	}

	if(pid != 0){
		//parent
		kill(_self,SIGTSTP);
		kill(pid,SIGTERM);
	}else{//child
		//get parent
		int p = getppid();
		//sleep
		while(kill(_parent,0))
			sleep(10);
		//kill if this process don't killed
		kill(p,SIGTERM);
		exit(0);
	}

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

static int fileWrite(int fd,void* buf,ssize_t size){
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

static int fileWriteStr(int fd,char* str){
	ssize_t writeSize = 0;

	do{
		ssize_t res = read(fd,&str[writeSize],1);
		if(res == 1)
			writeSize ++;
		else if(res == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
	}while((writeSize == 0 || str[writeSize-1] != '\0'));
}