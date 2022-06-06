//Miguel Pedroso 2019218176
//José Silva 2018296125
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>  //sincronizar fim da corrida
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <semaphore.h> // include POSIX semaphores
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#define PIPE_NAME "named_pipe"
#define BUFFER 50
//#define DEBUG
#define MAX_CARS 10
#define MAX_TEAMS 10		// max number of teams

typedef struct StrCarro
{
  int car_id;
  int speed;
  int reliability;
  double consumption;
  int number;
  int estado;   //Estado:  corrida(1) box(2) segurança(3) box(4) terminado(5) desistencia(6))
  double fuel;
  int position;
  int laps;
  int team;
  int avaria;
  int totalBoxStops;
  int totalMalfunctions;
  int totalRefuelings;
  int finalPosition;
  pthread_cond_t condCar;
}StrCarro;

typedef struct equipa{
  StrCarro cars[MAX_CARS];
  int infoBox;
  char nome[20];
  int carCount;
  int carsInReserve;
  int equipa;
  pthread_t threads[MAX_CARS];
  int channel[2];
  int finishedCars;
  pthread_mutex_t mtxTeam;
} equipa;

typedef struct mem_structure
{
  int carNumbers [MAX_TEAMS*MAX_CARS];   //first verify  if number exists and then create team
  equipa teams[MAX_TEAMS];
  //int infoCar[MAX_TEAMS][MAX_CARS];
  pid_t teams_pid[MAX_TEAMS], malfunction_manager;
  int mqid;
  int totalCarsRace, startedTeams, restart,teamsFinished;
  int start,time_units_second,lap_distance,lap_count,team_count,max_num_cars,breakdown_check_timer,min_pit,max_pit,fuel_capacity;
  int totalFinishedCars;
  pthread_mutex_t mtxMQ;
  pthread_mutex_t mtxMain;
} mem_structure;

typedef struct
{
  long mtype;
  int malfuction;
} failure_msg;

      //race manager creates team manager that creates car threads
int shmid;
int fd, fd;
pid_t race_manager, simulator;
mem_structure *memory;

sem_t *mutex;
sem_t *malfunction_mutex;
sem_t *stop_writers;
FILE * fi;
sigset_t block_set;


void printer(char *string, int flag);
void cleanup(int signo);
void monitor();
int* config(void);
void race_manager_code (void);
void malfunction_manager_code(void);
void team_manager_code(int equipa);
void* carro(void *threadid);
void current_time(int flag);
void handl_sigs();
void end_race(int signum);
void statistics();

  void printPosition(int pos)
  {

    for(int i=0;i<memory->team_count;i++)
    {
      char *teamName=memory->teams[i].nome;
      for(int j=0;j<memory->teams[i].carCount;j++)
      {
        if(memory->teams[i].cars[j].finalPosition==pos)
        {
          char est[100];
          sprintf(est,"Car %d from team %s finished in %d position, had %d malfuctions and %d refuelings!\n",memory->teams[i].cars[j].number,teamName,pos,memory->teams[i].cars[j].totalMalfunctions,memory->teams[i].cars[j].totalRefuelings);
          printer(est,0);
          return;
        }
      }
    }
  }

  void printCarsThatGaveUp(int lap)
  {
    for(int i=0;i<memory->team_count;i++)
    {
      char *teamName=memory->teams[i].nome;
      for(int j=0;j<memory->teams[i].carCount;j++)
      {
        if(memory->teams[i].cars[j].laps==lap)
        {
          char est[100];
          sprintf(est,"Car %d from team %s gave up on %d lap, had %d malfuctions and %d refuelings!\n",memory->teams[i].cars[j].number,teamName,lap,memory->teams[i].cars[j].totalMalfunctions,memory->teams[i].cars[j].totalRefuelings);
          printer(est,0);
        }
      }
    }
  }

  void cleanup(int signo) // clean up resources by pressing Ctrl-C
  {

    if(signo == SIGINT){
      kill(race_manager,SIGUSR1);
      printer("SIGNAL SIGINT RECEIVED",0);
      printer("CLOSING",0);
      sleep(4);
      sem_close(mutex);
    	sem_unlink("MUTEX");
      printf("Killing the teams\n");
      int i = 0;
      while (i < (memory->team_count)){
            //printf("Team pid [%d]",memory->teams_pid[i]);
        		kill(memory->teams_pid[i++], SIGKILL);
          }
      kill(race_manager,SIGKILL);
      #ifdef DEBUG
      printf("Race manager killed\n");
      #endif
      #ifdef DEBUG
      printf("Removing named pipe...\n");
      #endif
      close(fd);
    	unlink(PIPE_NAME);

      if (memory->mqid == -1) {
        #ifdef DEBUG
    		printf("Message queue does not exists.\n");
        #endif
    	}

    	if (msgctl(memory->mqid, IPC_RMID, NULL) == -1) {
    		fprintf(stderr, "Message queue could not be deleted.\n");
    	}
      kill(memory->malfunction_manager,SIGKILL);
    	if (shmid >= 0) // remove shared memory
    		shmctl(shmid, IPC_RMID, NULL);
      msgctl(memory->mqid,IPC_RMID,NULL);
      #ifdef DEBUG
      printf("Message queue deleted\n");
      #endif
    	printf("SHUTDOWN...\n");
    	exit(0);

    }
    if(signo == SIGTSTP){
      printer("SIGNAL SIGTSTP RECEIVED",0);
      kill(race_manager,SIGUSR1);
      //printer("sent signal to race manager",0);
    }
  }

  int* config(void)
  {
    FILE * fp;
		static int array[9];
		char *token;
		int i = 0;
    fp = fopen("config_SO.txt", "r");
    if (fp == NULL) {
      perror("Failed: ");
    }
    char buffer[10];
	    while (fgets(buffer, 10, fp))
    {
        buffer[strcspn(buffer, "\n")] = 0;
				token = strtok(buffer,",");
				array[i]= atoi(token);   //Convert token (string) to int
				i++;
				token = strtok(NULL,",");
				if (token!=NULL){
				 array[i]=atoi(token);
				 i++;
				}
    }
    fclose(fp);
    return array;
  }

  void end_race(int signum)
  {
    printer("Sigsur1 received!",1);
    if(memory->start == 0){
      printer("Race isnt started",0);
    }
    else{
      memory->start = 0;
      printer("Ending race",0);
      sleep(2);
      statistics();
      memory->restart = 1;
    }
  }


  int commandValidation(char arr[5][20], int count)
  {
  int spe;
  int r ;
  double co ;
  int num ;
  char str[200];

  if((num = atoi(arr[1]))== 0){

     printf("Conversion of car number failed\n");
     return 0;
   }

  if((spe = atoi(arr[2]))== 0){
    printf("Car number inserted incorrectly\n");
    return 0;
  }
  if((co = atof(arr[3]))== 0){
    printf("Comsumption inserted incorrectly\n");
    return 0;
  }
  if((r = atoi(arr[4]))== 0){
    printf("Reliability inserted incorrectly\n");
    return 0;
  }
  //printf("Max_num_cars: %d\n",memory->max_num_cars);
  int checker = -1;
  for( int k = 0 ;k < memory->team_count; k++)
  {
    if (strcmp(arr[0],memory->teams[k].nome)== 0)
    {
      //printf("Team already exists\n");
      checker = k;
      if (memory->teams[k].carCount == memory->max_num_cars)
      {
        printer("Max number of cars in the team already reached\n",0);
        return 0;
      }
      break;
    }
  }
         // variavel   que conta os carros ja adicionados a corrida
  for (int v = 0; v < memory->totalCarsRace; v++){
    if(num == memory->carNumbers[v]){
      printf("Car with that number already exists in the race\n");
      return 0;
    }
  }
  if (checker != -1){           //team already exists
      count = checker;
  }
  else if (count >= memory->team_count){
    printer("Cant had more teams, maximum number reached",0);
    return 0;
  }
  else{
      strcpy(memory->teams[count].nome,arr[0]);
      memory->teams[count].equipa=count;
      memory->teams[count].carsInReserve=0;
      memory->teams[count].infoBox=1;
  }

  memory->carNumbers[memory->totalCarsRace] = num;
  memory->totalCarsRace += 1;


  memory->teams[count].cars[memory->teams[count].carCount].car_id=memory->teams[count].carCount;
  memory->teams[count].cars[memory->teams[count].carCount].number = num;
  memory->teams[count].cars[memory->teams[count].carCount].speed = spe;
  memory->teams[count].cars[memory->teams[count].carCount].reliability = r;
  memory->teams[count].cars[memory->teams[count].carCount].consumption = co;
  memory->teams[count].cars[memory->teams[count].carCount].team = count;
  memory->teams[count].cars[memory->teams[count].carCount].fuel=memory->fuel_capacity;
  memory->teams[count].cars[memory->teams[count].carCount].laps=0;
  memory->teams[count].cars[memory->teams[count].carCount].position=0;
  memory->teams[count].cars[memory->teams[count].carCount].avaria=0;
  memory->teams[count].cars[memory->teams[count].carCount].totalBoxStops=0;
  memory->teams[count].cars[memory->teams[count].carCount].totalRefuelings=0;
  memory->teams[count].cars[memory->teams[count].carCount].totalMalfunctions=0;
  memory->teams[count].cars[memory->teams[count].carCount].finalPosition=0;

  //printf("Car %d added successfully to team %s(%d)\n",memory->teams[count].cars[memory->teams[count].carCount].number,memory->teams[count].nome,memory->teams[count].cars[memory->teams[count].carCount].team);
  //printf("Car id:%d\n",memory->teams[count].cars[memory->teams[count].carCount].car_id);
  //printf("Fuel:%f\n",memory->teams[count].cars[memory->teams[count].carCount].fuel);
  sprintf(str,"NEW CAR LOADED => Team: %s, Num: %d, Speed: %d, Reliability: %d, Consumption: %.3f",memory->teams[count].nome,memory->teams[count].cars[memory->teams[count].carCount].number,spe,r,co);
  printer(str,0);
  memory->teams[count].carCount += 1;
  if (count == checker) return 0;
  else return 1;
    }

  void start_teams(){

  }
  void statistics()
  {
    for(int i=1;i<=memory->totalFinishedCars;i++)
    {
      printPosition(i);
    }

    for(int i=9;i>=0;i--)
    {
      printCarsThatGaveUp(i);
    }
      memory->teamsFinished = 0;
      memory->totalFinishedCars = 0;
      for(int i = 0; i < memory->team_count; i++)
      {
      kill(memory->teams_pid[i],SIGUSR1);
      }
  }


	void race_manager_code (void)
	{
    signal(SIGUSR1,end_race);
    memory->startedTeams = 0;
		printer("RACE MANAGER STARTED",0);
		int fd;
    int pid;
		char string[BUFFER];
		if ((fd = open(PIPE_NAME, O_RDONLY)) < 0)
    {
			perror("Cannot open pipe for reading: ");
			exit(0);
		}
    else
    {
      #ifdef DEBUG
      printer("Named pipe creat successfully!",1);
      #endif
    }

    for(int i = 0; i< memory->team_count; i++)//creates team_count unamed pipes
    {
      pipe(memory->teams[i].channel);
      #ifdef DEBUG
      printer("Unnamed pipes created...",1);
      #endif
    }
    if (pthread_mutex_init(&(memory->mtxMain), NULL) != 0)
    {
        printf("\n Mained mutex failed\n");
        exit(0);
    }
    int m = 0;
    while (m < memory->team_count)   // defined by user in config file
    {
      if ((pid = fork()) == 0)  //creates team manager processes
      {
        team_manager_code(m);
        exit(0);
      }
      else if (memory->teams_pid[m] == -1)
      {
        perror("Failed to create team manager process\n");
        exit(1);
      }
      memory->teams_pid[m] = pid;
      //printf("Team [%d]\n",memory->teams_pid[m]);
      ++m;
    }

    #ifdef DEBUG
    printer("Team managers created!",1);
    #endif


    	while (1)//receives commands from main, using named pipe
      {
          //printf("\nReady to read\n");
          //printf("TeamCount value: %d\n",memory->startedTeams);
          //printf("Start: %d\n",memory->start);
    			read(fd, &string, BUFFER);
    			//printf("%s\n",string);
          if(memory->start == 1)//if race already started deny commands
          {
            printer("Rejected, race already started!",1);
            continue;
          }

          if(strncmp(string,"START RACE!",10) == 0)//if command is START RACE
          {
            printer("NEW COMMAND RECEIVED: START RACE",0);
            if(memory->startedTeams == memory->team_count && memory->start == 0)
            {
              memory->start = 1;//set flag start in shared memoty to 1
              printf("Race STARTED !!!!\n");
              kill(memory->malfunction_manager,SIGUSR2);//unlocks malfunction_manager
              for(int i = 0; i < memory->team_count; i++)
              {
                kill(memory->teams_pid[i],SIGUSR2);
              }
            }
            else if (memory->startedTeams < memory->team_count)
            {
              printer("CANNOT START, NOT ENOUGH TEAMS",0);
            }
            continue;
          }


          const char s[2] = ",";
          char *token;
          char arr[5][20];
          memset(arr, 0, sizeof arr);
          token = strtok(string, s);
          int i  = 0;
          while( token != NULL )
          {
              //printf( "%s\n", token );
              strncpy(arr[i],token,strlen(token));
              arr[i][strlen(token)] = '\0';
              token = strtok(NULL, s);
              i++;
          }

           int a = commandValidation(arr, memory->startedTeams);
           //printf("Command validator result: %d \n",a);
           memory->startedTeams += a;
    	}
	}

  void malfunction_start(){
    printer("Acess received, beginning malfuction generation",1);
  }


  void malfunction_manager_code(void){
    signal(SIGUSR2,malfunction_start);
    memory->mqid = msgget(IPC_PRIVATE, IPC_CREAT|0700);
    failure_msg my_msg;
    int random;
    char string[100];
    if (memory->mqid < 0)
      {
        perror("Creating message queue");
        exit(0);
      }                                     //on car thread if msgrcv != -1 switch into failure mode
    #ifdef DEBUG
        printf("Message queue created by malfunction manager\n");           //use IPC_NOWAIT on msgrcv
    #endif
    if (pthread_mutex_init(&(memory->mtxMQ), NULL) != 0)
    {
        printf("\n mutex for MQ failed\n");
        exit(0);
    }
      sleep(1);
      while(1)
      {
        #ifdef DEBUG
          printer("Malfunction manager waiting for race start to start generating failures",1);
        #endif
        pause();// waits for race manager signal to start the race

        while(memory->start == 1)
        {
          //printf("Malfunction timer: %d\n",memory->breakdown_check_timer);
      		sleep(memory->breakdown_check_timer);
          //printer("Any problems?\n",1);
          srand(time(0));
      		for(int i = 0; i < memory->team_count; i++)
          {
            for(int j = 0; j < memory->teams[i].carCount; j++)
            {

              if(memory->teams[i].cars[j].estado == 1)
              {               //if car is racing and has no problems already
                random = (rand() %(100)) + 1;
                //printf("Probablilty of failure for car %d from team %d:%d/%d\n\n",j,i,random,memory->teams[i].cars[j].reliability);
                if(random > memory->teams[i].cars[j].reliability)
                {
                  my_msg.mtype = (memory->teams[i].cars[j].number);
                  my_msg.malfuction=1;
                  #ifdef DEBUG
                  sprintf(string,"New problem in car %d from team %s!\n",memory->teams[i].cars[j].number,memory->teams[i].nome);
                  printer(string,0);
                  #endif
                  pthread_mutex_lock(&(memory->mtxMQ));
                  msgsnd(memory->mqid, &my_msg, sizeof(failure_msg)-sizeof(long), 0);
                  pthread_mutex_unlock(&(memory->mtxMQ));
                }

            }
    		  }
    	   }
       }
       #ifdef DEBUG
       printer("Malfunction manager ending for this race",1);
       #endif
   }


}

  void* carro(void *car)
  {
    StrCarro *carro_data=(StrCarro *) car;//estrutura carro passada para o thread para este saber que carro e
    failure_msg msg;//estrutura da mensagem do malfunction_manager



    //informaçao acedida a estrutura passada para o thread, não necessita de sincronizaçao(exclusiva para cada thread)
    int maxFuel=memory->fuel_capacity;
    int carNumber=carro_data->number;
    int equipa=carro_data->team;
    int carId=carro_data->car_id;
    double consumption=carro_data->consumption;
    int time_units_second=memory->time_units_second;
    int speed=carro_data->speed;
    int lap_count=memory->lap_count;
    int lap_distance=memory->lap_distance;//acedida por vários threads antes da corrida começar, não é alterada logo não necessita de sincronizaçao
    char *teamName=memory->teams[equipa].nome;
    #ifdef DEBUG
    printf("Team:%d, Car:%d, Consumption:%f, Time:%d, Speed:%d\n",equipa,carId,consumption,time_units_second,speed);
    #endif
    //close(memory->teams[equipa].channel[0]);
    int x=(9*lap_distance)/10;
    memory->teams[equipa].cars[carId].estado=1;



    char message[100];
    sprintf(message,"Car %d from team %s started running!\n",carNumber,teamName);
    printer(message,0);
    close(memory->teams[equipa].channel[0]);
    while(1)
    {
      char estado[100];
      if(memory->teams[equipa].cars[carId].estado==1)//corrida
      {
        msg.malfuction=0;
        pthread_mutex_lock(&(memory->mtxMQ));
        msgrcv(memory->mqid,&msg,sizeof(msg)-sizeof(long),carNumber,IPC_NOWAIT);
        pthread_mutex_unlock(&(memory->mtxMQ));
        if(msg.malfuction==1)//verificamos se ficamos com uma avaria
        {
          memory->teams[equipa].cars[carId].estado=3;//seguranca
          sprintf(estado,"Car %d from team %s changed to state %d!(Malfunction)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado);
          printer(estado,0);
          //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));

          memory->teams[equipa].cars[carId].avaria=1;
          //carsInReserve++;

          goto update;
        }


        //movemos a posiçao do carro e gastamos combustivel
        memory->teams[equipa].cars[carId].fuel=memory->teams[equipa].cars[carId].fuel-consumption;
        memory->teams[equipa].cars[carId].position=memory->teams[equipa].cars[carId].position+(speed/time_units_second);
        //se o combustivel durar apenas para mais 4 voltas
        if(memory->teams[equipa].cars[carId].fuel<=((4*lap_distance)/speed)*consumption)
        {
          pthread_mutex_lock(&(memory->teams[equipa].mtxTeam));
          #ifdef DEBUG
          printf("Car %d from team %s trying to enter box(%d), position = %d\n",carNumber,teamName,memory->teams[equipa].infoBox,memory->teams[equipa].cars[carId].position);
          #endif
          if((memory->teams[equipa].infoBox==1)&&(memory->teams[equipa].cars[carId].position==lap_distance||memory->teams[equipa].cars[carId].position>=x))//se a box estiver vazia entra
          {
            #ifdef DEBUG
            printf("Car %d from team %s is inside the box\n",carNumber,teamName);
            #endif
            memory->teams[equipa].cars[carId].estado=2;
            sprintf(estado,"Car %d from team %s changed to state %d box was in state %d!(Fuel only for 4 more laps)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado,memory->teams[equipa].infoBox);
            printer(estado,0);
            //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
            memory->teams[equipa].infoBox=3;//ocupada;
            pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));


            goto box;
          }
          pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));

          //se o combustivel durar apenas para mais 2 voltas
          if(memory->teams[equipa].cars[carId].fuel<=((2*lap_distance)/speed)*consumption)
          {
            memory->teams[equipa].cars[carId].estado=3;//vamos para estado de seguranca
            sprintf(estado,"Car %d from team %s changed to state %d!(Fuel only for 2 more laps)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado);
            printer(estado,0);
            //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
            goto update;
          }
        }
        goto update;
      }
      box:if(memory->teams[equipa].cars[carId].estado==2)//box
      {
        //espera que o gestor de equipa ateste e/ou repare o carro
        pthread_mutex_lock(&(memory->teams[equipa].mtxTeam));
        while(memory->teams[equipa].cars[carId].fuel!=maxFuel)
        {
          pthread_cond_wait(&(memory->teams[equipa].cars[carId].condCar),&(memory->teams[equipa].mtxTeam));
        }
        #ifdef DEBUG
        printf("Car %d from team %s leaving box(%d)\n",carNumber,teamName,memory->teams[equipa].infoBox);
        #endif
        memory->teams[equipa].infoBox=1;
        sprintf(estado,"Car %d from team %s changed to state %d leaving box in state %d!\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado,memory->teams[equipa].infoBox);
        printer(estado,0);
        //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
        pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));
        goto update;
      }
      if(memory->teams[equipa].cars[carId].estado==3)//seguranca
      {
        memory->teams[equipa].cars[carId].fuel=memory->teams[equipa].cars[carId].fuel-(0.4*consumption);
        memory->teams[equipa].cars[carId].position=memory->teams[equipa].cars[carId].position+(0.3*(speed/time_units_second));
        pthread_mutex_lock(&(memory->teams[equipa].mtxTeam));
        #ifdef DEBUG
        printf("Car %d from team %s trying to enter box(%d), position = %d\n",carNumber,teamName,memory->teams[equipa].infoBox,memory->teams[equipa].cars[carId].position);
        #endif
        if(memory->teams[equipa].infoBox==1&&(memory->teams[equipa].cars[carId].position==lap_distance||memory->teams[equipa].cars[carId].position>=x))//se a box estiver vazia entra
        {
          #ifdef DEBUG
          printf("Car %d from team %s is inside the box\n",carNumber,teamName);
          #endif
          memory->teams[equipa].cars[carId].estado=2;
          sprintf(estado,"Car %d from team %s changed to state %d box was in state %d!(Was in safety mode)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado,memory->teams[equipa].infoBox);
          printer(estado,0);
          //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
          memory->teams[equipa].infoBox=3;//ocupada;
          pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));


          goto box;
        }
        pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));
        goto update;

      }
      if(memory->teams[equipa].cars[carId].estado==4)//terminado
      {
        //guardar a posicao em que acabou;
        #ifdef DEBUG
        printf("Car %d from team %s finished\n\n",carNumber,teamName);
        #endif
        pthread_mutex_lock(&(memory->teams[equipa].mtxTeam));
        memory->teams[equipa].finishedCars++;
        pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));
        pthread_mutex_lock(&(memory->mtxMain));
        memory->totalFinishedCars++;
        memory->teams[equipa].cars[carId].finalPosition=memory->totalFinishedCars;
        pthread_mutex_unlock(&(memory->mtxMain));
        pthread_exit(NULL);
      }
      if(memory->teams[equipa].cars[carId].estado==5)//desistencia
      {
        #ifdef DEBUG
        printf("Car %d from %s has given up on its %d lap\n\n",carNumber,teamName,memory->teams[equipa].cars[carId].laps);
        #endif
        pthread_mutex_lock(&(memory->teams[equipa].mtxTeam));
        memory->teams[equipa].finishedCars++;
        pthread_mutex_unlock(&(memory->teams[equipa].mtxTeam));
        pthread_exit(NULL);
      }

      update:

      if(memory->teams[equipa].cars[carId].fuel<=0)
      {
        memory->teams[equipa].cars[carId].estado=5;
        sprintf(estado,"Car %d from team %s changed to state %d!(Ran out of fuel)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado);
        printer(estado,0);
        //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
        memory->teams[equipa].cars[carId].fuel=0;
      }


      if(memory->teams[equipa].cars[carId].laps>=lap_count)
      {
        memory->teams[equipa].cars[carId].estado=4;
        sprintf(estado,"Car %d from team %s changed to state %d!(Finished Race)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado);
        printer(estado,0);
        //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
      }

      if(memory->teams[equipa].cars[carId].position>=lap_distance)
      {
        memory->teams[equipa].cars[carId].position=memory->teams[equipa].cars[carId].position-lap_distance;
        memory->teams[equipa].cars[carId].laps++;
        #ifdef DEBUG
        printf("Car %d from %s is on the %d lap\n",carNumber,teamName,memory->teams[equipa].cars[carId].laps);
        #endif

        if(memory->start!=1)
        {
          memory->teams[equipa].cars[carId].estado=4;
          sprintf(estado,"Car %d from team %s changed to state %d!(Race stopped)\n",carNumber,teamName,memory->teams[equipa].cars[carId].estado);
          printer(estado,0);
          //write(memory->teams[equipa].channel[1], &estado, sizeof(estado));
        }
      }

      #ifdef DEBUG
      printf("Car %d from %s is on the %d lap, position %d with %f fuel and %d state\n",carNumber,teamName,memory->teams[equipa].cars[carId].laps,memory->teams[equipa].cars[carId].position,memory->teams[equipa].cars[carId].fuel,memory->teams[equipa].cars[carId].estado);
      #endif
      sleep(time_units_second);
    }
  }

  void start_threads(){
      printer("Starting the cars",1);

  }
  void restarter(){
     //printer("Restarting race",0);
  }

  void team_manager_code(int team)
	{
			//create car threads
      signal(SIGUSR2,start_threads);
      signal(SIGUSR1,restarter);
			char na[50];
      #ifdef DEBUG
      printf("Team manager %d started! Waiting for all the cars to be inserted by user!\n",team);
      #endif

      pause:
      printf("Team manager %d waiting for start signal\n",team);
      pause();
      sprintf(na, "Team %s iniciated! Has %d cars!", memory->teams[team].nome,memory->teams[team].carCount);
      printer(na,0);

      if (pthread_mutex_init(&(memory->teams[team].mtxTeam), NULL) != 0)
      {
          printf("\n mutex for team %d failed\n",team);
          exit(0);
      }


	   	int rc;
	   for(int i=0; i < memory->teams[team].carCount; i++ )//creates this team's cars and initializes car cond
	   {
				//printf("A criar carro %d da equipa %d\n",i,memory->teams[equipa].equipa);
	      rc = pthread_create(&(memory->teams[team].threads[i]), NULL,carro, &(memory->teams[team].cars[i]));
	      if (rc)
				{

	         printf("Error creating thread(car)%d from team %d",i,team);
					 fflush(stdout);
	         exit(-1);
	      }

        if (pthread_cond_init(&(memory->teams[team].cars[i].condCar), NULL) != 0)
        {
            printf("\n Cond for car %d failed\n",i);
            exit(0);
        }
	   }

     memory->teams[team].finishedCars=0;
     while(1)
     {
       if (memory->restart== 1){
         goto restart;
       }
       for(int i=0; i < memory->teams[team].carCount; i++ )
  	   {

         if(memory->teams[team].cars[i].estado==2&&memory->teams[team].infoBox==3)
         {
           #ifdef DEBUG
           printf("Reparing car %d from team %s\n",memory->teams[team].cars[i].number,teamName);
           #endif
           memory->teams[team].cars[i].totalBoxStops++;
           if(memory->teams[team].cars[i].avaria==1)
           {
             #ifdef DEBUG
             printf("Reparing car %d from team %s\n",memory->teams[team].cars[i].number,teamName);
             #endif
             srand(time(0));
             sleep((rand() % ((memory->max_pit) - (memory->min_pit) + 1)) + (memory->min_pit));
             memory->teams[team].cars[i].avaria=0;
             memory->teams[team].cars[i].totalMalfunctions++;
             #ifdef DEBUG
             printf("Car %d from team %s repaired\n",memory->teams[team].cars[i].number,teamName);
             #endif
           }
           #ifdef DEBUG
           printf("Refueling car %d from team %s\n",memory->teams[team].cars[i].number,teamName);
           #endif
           memory->teams[team].cars[i].fuel=memory->fuel_capacity;

           memory->teams[team].cars[i].position=0;
           memory->teams[team].cars[i].laps++;
           memory->teams[team].cars[i].totalRefuelings++;
           sleep(2*(memory->time_units_second));
           memory->teams[team].cars[i].estado=1;
           #ifdef DEBUG
           printf("Car %d from team %s refuelled\n",memory->teams[team].cars[i].number,teamName);
           #endif
           pthread_mutex_lock(&(memory->teams[team].mtxTeam));
           pthread_cond_broadcast(&(memory->teams[team].cars[i].condCar));
           pthread_mutex_unlock(&(memory->teams[team].mtxTeam));
         }

       }
       pthread_mutex_lock(&(memory->teams[team].mtxTeam));
       if(memory->teams[team].finishedCars==memory->teams[team].carCount)
       {
         pthread_mutex_unlock(&(memory->teams[team].mtxTeam));
         break;
       }
       pthread_mutex_unlock(&(memory->teams[team].mtxTeam));
       sleep(memory->time_units_second);
     }


		 for(int i=0; i < memory->teams[team].carCount; i++ )//waits for cars to end
	   {
			 pthread_join(memory->teams[team].threads[i],NULL);
       #ifdef DEBUG
       printf("thread(car)%d from team %s closed\n\n",memory->teams[team].cars[i].number,teamName);
       #endif

		 }
     memory->teamsFinished ++;
     //memory->totalFinishedCars += memory->teams[team].carCount;
     if (memory->teamsFinished == memory->team_count){
       kill(simulator,SIGINT);
     }

     pause();
     for(int i=0; i < memory->teams[team].carCount; i++ )
	    {
			 memory->teams[team].cars[i].laps= 0;
       memory->teams[team].cars[i].position= 0;
       memory->teams[team].cars[i].fuel= memory->fuel_capacity;
		 }

     goto pause;   //if they receive a signal, they must restart
     //wait and see if evryone finishes

     pthread_mutex_destroy(&(memory->teams[team].mtxTeam));
     for(int i=0; i < memory->teams[team].carCount; i++ )
     {
       pthread_cond_destroy(&(memory->teams[team].cars[i].condCar));
     }
     return;

     restart:
     for(int i=0; i < memory->teams[team].carCount; i++ )//waits for cars to end
	    {
			 pthread_join(memory->teams[team].threads[i],NULL);
       //printf("thread(car)%d from team %s killed\n\n",memory->teams[team].cars[i].number,teamName);

		 }

     for(int i=0; i < memory->teams[team].carCount; i++ )//waits for cars to end
	    {
			 memory->teams[team].cars[i].laps= 0;
       memory->teams[team].cars[i].position= 0;
       memory->teams[team].cars[i].fuel= memory->fuel_capacity;
		 }
     memory->restart = 0;
     //memory->teamsFinished ++;
     goto pause;


	}

 void current_time(int flag ){
	 char buf[256] = {0};
   time_t rawtime = time(NULL);

    if (rawtime == -1) {
        puts("The time() function failed");
    }

    struct tm *ptm = localtime(&rawtime);

    if (ptm == NULL) {

        puts("The localtime() function failed");

    }
	 strftime(buf, 256, "%T ", ptm);
	 if (flag == 0){
		 printf("%s",buf);
	 	 fprintf(fi, "%s",buf);
	 }
	 else
	 		printf("%s",buf);


 }

 void printer(char *string, int flag){  // Flag = 0 -> prints on stdout and log   Flag = 1 -> prints only on stdout
	 if (flag == 0){
		 sem_wait(mutex);
		 current_time(0);
		 fprintf(fi, "%s\n",string);
		 printf("%s\n",string);
	 	 fflush(fi);
		 fflush(stdout);
	 	 sem_post(mutex);
	 }
	 else{
		 sem_wait(mutex);
		 current_time(1);
		 printf("%s\n",string);
     fflush(stdout);
		 sem_post(mutex);
	 }

 }

int main() {

  handl_sigs();
  simulator = getpid();
	fi = fopen("log.txt", "w");
	sem_unlink("MUTEX");
	mutex=sem_open("MUTEX",O_CREAT|O_EXCL,0700,1);
	printer("SIMULATOR STARTING",1);
	int *ar;
	ar =config();
  signal(SIGTSTP,SIG_IGN);
  signal(SIGINT,SIG_IGN);
	/*for (int i = 0;i < 9;i++){			//Print config data on ar array
		printf("%d ", *(ar+i));
	}
*/



	//Create shared memory
	if ((shmid = shmget(IPC_PRIVATE, sizeof(mem_structure), IPC_CREAT|0700))<0){
		perror("Error in shmget with IPC_CREAT\n");
		exit(1);
	}

	// Attach shared memory
	if((memory = ( mem_structure *) shmat(shmid, NULL, 0)) == (mem_structure *) -1){
		perror("Shmat error");
		exit(1);
	}
  #ifdef DEBUG
    printf("Shared memory attached\n");
  #endif


  //fills shared memory
  memory->start=0;
	memory->time_units_second= *(ar);
	memory->lap_distance = *(ar+1);
	memory->lap_count = *(ar+2);
	if (*(ar+3)>2 &&  *(ar+3)<11)
  {
		memory->team_count = *(ar+3);
	}
	else{
		printer("CANNOT START, NOT ENOUGH TEAMS: minimum of 3 teams required to start race\nExiting simulator...",0);
		shmctl(shmid, IPC_RMID, NULL);
		printer("PROGRAMA A TERMINAR...",0);
		exit(0);
	}
	memory->max_num_cars = *(ar+4);
	memory->breakdown_check_timer= *(ar+5);
	memory->min_pit = *(ar+6);
	memory->max_pit = *(ar+7);
	memory->fuel_capacity = *(ar+8);

	if ((mkfifo(PIPE_NAME, O_CREAT|O_EXCL|0600)<0) && (errno!= EEXIST)) {
		perror("Cannot create pipe: ");
		cleanup(SIGINT);
	}
	else{
		#ifdef DEBUG
		printf("Named pipe created with success\n");
		#endif
	}


	if ((race_manager = fork()) == 0) {
		#ifdef DEBUG
		printf("[%d] Race Manager Process created\n",getpid());
		#endif
    race_manager_code();
    exit(0);
  }
  else if (race_manager == -1) {
    perror("Failed to create race manager process\n");
    exit(1);
  }
  pid_t malfunction_manager;
  if ((malfunction_manager = fork()) == 0)
	{
    memory->malfunction_manager = getpid();
		#ifdef DEBUG
		printer("MALFUNCTION MANAGER STARTED",1);
		#endif
    malfunction_manager_code();
    exit(0);
  }

  else if (malfunction_manager == -1) {
    perror("Failed to create malfunction manager process\n");
    exit(1);
  }


	if ((fd = open(PIPE_NAME, O_WRONLY)) < 0) {
		perror("Cannot open pipe for writing: ");
		exit(0);
	}

	//printf("im here\n");
  signal(SIGINT,cleanup);
  signal(SIGTSTP,cleanup);
	char string[BUFFER];
  char st[100];
	printer("Add the desired cars",1);
	printer("{nome da equipa},{número carro},{metros por unidade de tempo},{litros por unidade de tempo},{% de fiabilidade}",1);
	printer("To start the race, write 'START RACE!'",1);
  while (1)
  {
		fgets(string, 50, stdin);
    if(memory->start!=1)
    {
      write(fd, &string, BUFFER);
    }
    else
    {
      sprintf(st,"NEW COMMAND RECEIVED: %s",string);
      printer(st,0);
      if(strcmp(string, "START RACE")==0)
      {
        printer("RACE ALREADY STARTED",1);
      }
      else
      {
        
        printer("WRONG COMMAND",1);
        printer(string,0);
      }
      sleep(1);
    }
  }
}


void handl_sigs()
{
sigfillset(&block_set);
sigdelset (&block_set, SIGINT);
sigdelset (&block_set, SIGTSTP);
sigdelset (&block_set, SIGUSR1);
sigdelset (&block_set, SIGUSR2);
sigprocmask (SIG_BLOCK, &block_set, NULL);
#ifdef DEBUG
  printf("Blocking most signals\n");
#endif
}
