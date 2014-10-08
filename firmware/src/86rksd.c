// SD Controller for Computer "Radio 86RK" / "Apogee BK01"
// (c) 10-05-2014 vinxru (aleksey.f.morozov@gmail.com)
// Ported to avr-gcc: <andy.karpov@gmail.com> at 07.10.2014

// уже определено в Makefile
//#define F_CPU 8000000UL        //freq 8 MHz

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <string.h>

#include "common.h"
#include "sd.h"
#include "fs.h"
#include "proto.h"

#define O_OPEN   0
#define O_CREATE 1
#define O_MKDIR  2
#define O_DELETE 100
#define O_SWAP   101

#define ERR_START       0x40
#define ERR_WAIT        0x41
#define ERR_OK_DISK         0x42
#define ERR_OK_CMD          0x43
#define ERR_OK_READ         0x44
#define ERR_OK_ENTRY        0x45
#define ERR_OK_WRITE        0x46
#define ERR_OK_RKS          0x47
#define ERR_READ_BLOCK      0x4F

uint8_t buf[512];
uint8_t rom[128];

/*******************************************************************************
* Для удобства                                                                 *
*******************************************************************************/

extern void RomEmu(void);

void recvBin(uint8_t* d, WORD l) {
  for(; l; --l) {
    *d++ = wrecv();
  }  
}

void recvString(void) {
  uint8_t c;
  uint8_t* p = buf;       
  do {
    c = wrecv();
    if(p != buf + FS_MAXFILE) *p++ = c; else lastError = ERR_RECV_STRING;
  } while(c);
}

void sendBin(uint8_t* p, WORD l) {
  for(; l; l--)
    send(*p++);
}

void sendBinf(const char* d, uint8_t l) {
  for(; l; --l) {
      uint8_t x = pgm_read_byte(*d++);
      send(x);
  }
}

/*******************************************************************************
* Отправка всех блоков файла                                                   *
*******************************************************************************/

WORD readLength;

void readInt(char rks) { 
  WORD readedLength, lengthFromFile;        
  uint8_t tmp;
  uint8_t* wptr;

  while(readLength) { 
    // Расчет длины блока (выравниваем чтение на сектор)
    if(fs_tell()) return;
    readedLength = 512 - (fs_tmp % 512);
    if(readedLength > readLength) readedLength = readLength;

    // Уменьшаем счетчик
    readLength -= readedLength;

    // Читаем блок
    if(fs_read0(buf, readedLength)) return;

    // Заголовок RKS файла
    wptr = buf;
    if(rks) { // Если rks=1, перед вызовом надо проверить, что бы readLength>4 и fs_file.ptr=0, иначе может быть злостный сбой
      rks = 0;
      
      // У апогея числа перепутаны
      tmp=buf[0], buf[0]=buf[1]; buf[1]=tmp;
      tmp=buf[2], buf[2]=buf[3]; buf[3]=tmp;

      // Посылаем адрес загрузки
      send(ERR_OK_RKS);
      sendBin(buf, 2);    
      send(ERR_WAIT);

      // Корректируем указатели
      wptr += 4;
      readedLength -= 4;

      // Длина из файла
      lengthFromFile = *(WORD*)(buf+2) - *(WORD*)(buf) + 1;

      // Корректируем длину  
      if(readedLength > lengthFromFile) {
        readedLength = lengthFromFile;
      } else {          
        lengthFromFile -= readedLength;
        if(readLength > lengthFromFile) lengthFromFile = readedLength;
      }
    }  

    // Отправляем блок
    send(ERR_READ_BLOCK);    
    sendBin((uint8_t*)&readedLength, 2);
    sendBin(wptr, readedLength);
    send(ERR_WAIT);
  }

  // Если все ОК
  if(!lastError) lastError = ERR_OK_READ;
}

/*******************************************************************************
* Версия команд контроллера                                                    *
*******************************************************************************/

void cmd_ver(void) {
  sendStart(1);
    
  // Версия + Производитель
  //sendBinf(PSTR("V1.0 10-05-2014 "), 16);
  sendBinf(PSTR("V1.1 RK86-SRAM  "), 16);
              //0123456789ABCDEF
}

/*******************************************************************************
* BOOT / EXEC                                                                  *
*******************************************************************************/

void cmd_boot_exec(void) {
  // Файл по умолчанию
  if(buf[0]==0) strcpy_P((char*)buf, PSTR("boot/sdbios.rk"));

  // Открываем файл
  if(fs_open()) return;
  
  // Максимальный размер файла
  readLength = 0xFFFF;  
  if(fs_getfilesize()) return;
  if(readLength > fs_tmp) readLength = (WORD)fs_tmp;

  // Файлы RK должны быть длиной >4 байт. Мы заносим в readLength = 0 и программа
  // получает ERR_OK. Но так как она ждем ERR_OK_RKS, это будет ошибкой 
  if(readLength < 4) readLength = 0;

  readInt(/*rks*/1);  
}

void cmd_boot(void) { 
  sendStart(ERR_WAIT);
  buf[0] = 0;
  cmd_boot_exec();  
}

void cmd_exec(void) {     
  // Прием имени файла
  recvString();

  // Режим передачи и подтверждение
  sendStart(ERR_WAIT);
  if(lastError) return; // Переполнение строки
  
  cmd_boot_exec();    
}

/*******************************************************************************
* Начать/продолжить поиск файлов в папке                                       *
*******************************************************************************/

typedef struct {
    char    fname[11];    // File name
    uint8_t    fattrib;    // Attribute
    DWORD   fsize;        // File size
    union {
      struct {
        WORD    ftime;        // Last modified time
        WORD    fdate;        // Last modified date 
      };
      DWORD ftimedate;
    };
} FILINFO2;

void cmd_find(void) {
  WORD n;
  FILINFO2 info;              
  
  // Принимаем путь
  recvString();

  // Принимаем макс кол-во элементов
  recvBin((uint8_t*)&n, 2);

  // Режим передачи и подтверждение
  sendStart(ERR_WAIT);
  if(lastError) return;

  // Открываем папку
  if(buf[0] != ':') {
    if(fs_opendir()) return;
  }

  for(; n; --n) {
    /* Читаем очереной описатель */
    if(fs_readdir()) return;

    /* Конец */
    if(FS_DIRENTRY[0] == 0) {
      lastError = ERR_OK_CMD;
      return;
    }

    /* Сжимаем ответ для компьютера */
    memcpy(info.fname, FS_DIRENTRY+DIR_Name, 12);
    memcpy(&info.fsize, FS_DIRENTRY+DIR_FileSize, 4);
    memcpy(&info.ftimedate, FS_DIRENTRY+DIR_WrtTime, 4);
    //memcpy(memcpy(memcpy(info.fname, FS_DIRENTRY+DIR_Name, 12, FS_DIRENTRY+DIR_FileSize, 4), FS_DIRENTRY+DIR_WrtTime, 4);

    /* Отправляем */
    send(ERR_OK_ENTRY);
    sendBin((uint8_t*)&info, sizeof(info));
    send(ERR_WAIT);
  }

  /* Ограничение по размеру */  
  lastError = ERR_MAX_FILES; /*! Надо опеределать, что бы не было ложных ошибок */
}

/*******************************************************************************
* Открыть/создать файл/папку                                                   *
*******************************************************************************/

void cmd_open(void) {
  uint8_t mode;
 
  /* Принимаем режим */
  mode = wrecv();    

  // Принимаем имя файла
  recvString();

  // Режим передачи и подтверждение
  sendStart(ERR_WAIT);

  // Открываем/создаем файл/папку
  if(mode == O_SWAP) {
    fs_swap();
  } else
  if(mode == O_DELETE) {
    fs_delete();
  } else
  if(mode == O_OPEN) {
    fs_open();
  } else 
  if(mode < 3) {
    fs_open0(mode);
  } else {
    lastError = ERR_INVALID_COMMAND;
  }

  // Ок
  if(!lastError) lastError = ERR_OK_CMD;
}

/*******************************************************************************
* Переместить файл/папку                                                       *
*******************************************************************************/

void cmd_move(void) {
  recvString();
  sendStart(ERR_WAIT);
  fs_openany();
  sendStart(ERR_OK_WRITE);
  recvStart();
  recvString();
  sendStart(ERR_WAIT);
  if(!lastError) fs_move0();
  if(!lastError) lastError = ERR_OK_CMD;
}

/*******************************************************************************
* Установить/прочитать указатель чтения                                        *
*******************************************************************************/

void cmd_lseek(void) {
  uint8_t mode;
  DWORD off;

  // Принимаем режим и смещение
  mode = wrecv();    
  recvBin((uint8_t*)&off, 4);    

  // Режим передачи и подтверждение
  sendStart(ERR_WAIT);

  // Размер файла
  if(mode==100) {
    if(fs_getfilesize()) return;
  }

  // Размер диска  
  else if(mode==101) {
    if(fs_gettotal()) return;
  }
 
  // Свободное место на диске
  else if(mode==102) {
    if(fs_getfree()) return;
  }

  else {
    /* Устаналиваем смещение. fs_tmp сохраняется */
    if(fs_lseek(off, mode)) return;
  }

  // Передаем результат
  send(ERR_OK_CMD);
  sendBin((uint8_t*)&fs_tmp, 4);  
  lastError = 0; // На всякий случай, результат уже передан
}

/*******************************************************************************
* Прочитать из файла                                                           *
*******************************************************************************/

void cmd_read(void) {
  DWORD s;

  // Длина
  recvBin((uint8_t*)&readLength, 2);

  // Режим передачи и подтверждение
  sendStart(ERR_WAIT);

  // Ограничиваем длину длиной файла
  if(fs_getfilesize()) return;
  s = fs_tmp; 
  if(fs_tell()) return;
  s -= fs_tmp;
                    
  if(readLength > s)
    readLength = (WORD)s;

  // Отправляем все блоки файла
  readInt(/*rks*/0);
}

/*******************************************************************************
* Записать данные в файл                                                       *
*******************************************************************************/

void cmd_write(void) {
  // Аргументы
  recvBin((uint8_t*)&fs_wtotal, 2); 

  // Ответ
  sendStart(ERR_WAIT);
           
  // Конец файла
  if(fs_wtotal==0) {
    fs_write_eof();
    lastError = ERR_OK_CMD;
    return;
  }
  
  // Запись данных
  do {
    if(fs_write_start()) return;

    // Принимаем от компьюетра блок данных
    send(ERR_OK_WRITE);
    sendBin((uint8_t*)&fs_file_wlen, 2);
    recvStart();    
    recvBin(fs_file_wbuf, fs_file_wlen);
    sendStart(ERR_WAIT);

    if(fs_write_end()) return;
  } while(fs_wtotal);

  lastError = ERR_OK_CMD;
}

/*******************************************************************************
* Главная процедура                                                            *
*******************************************************************************/

void error(void) {
  for(;;) {
    PORTB |= _BV(PB0);
    _delay_ms(100);
    PORTB &=~_BV(PB0);
    _delay_ms(100);
  }
}

int main(void) {
  uint8_t c;
    
  DATA_OUT            // Шина данных (DDRD)
  DDRC  = 0b00000000; // Шина адреса
  DDRB  = 0b00101101; // Шина адреса, карта и светодиод
  PORTB = 0b00010001; // Подтягивающий резистор на MISO и светодиод  

  // Пауза, пока не стабилизируется питание
  _delay_ms(100);

  // Запуск файловой системы
  if(fs_init()) error();
  strcpy_P((char*)buf, PSTR("boot/boot.rk"));
  if(fs_open()) error();
  if(fs_getfilesize()) error();
  if(fs_tmp < 7) error();
  if(fs_tmp > 128) error();
  if(fs_read0(rom, (WORD)fs_tmp)) error();  
                    
  // Гасим светодиод
  PORTB |= _BV(PB0);
  
  while(1) {

    // Эмуляция ПЗУ
    RomEmu();

    // Зажигаем светодиод
    PORTB |= _BV(PB0);

    // Проверяем наличие карты
    sendStart(ERR_START);
    send(ERR_WAIT);
    if(fs_check()) {
      send(ERR_DISK_ERR);
    } else {
      send(ERR_OK_DISK);
      recvStart();
      c = wrecv();
      
      // Сбрасываем ошибку
      lastError = 0;
    
      // Принимаем аргументы 
      switch(c) {
        case 0:  cmd_boot();         break; 
        case 1:  cmd_ver();          break;
        case 2:  cmd_exec();         break; 
        case 3:  cmd_find();         break;
        case 4:  cmd_open();         break;     
        case 5:  cmd_lseek();        break;     
        case 6:  cmd_read();         break;     
        case 7:  cmd_write();        break; 
        case 8:  cmd_move();         break;
        default: lastError = ERR_INVALID_COMMAND;      
      }
    
      // Вывод ошибки
      if(lastError) sendStart(lastError);
    }

    // Порт рабоатет на выход
    wait();
    DATA_OUT
    
    // Гасим светодиод
    PORTB &=~_BV(PB0);
  } 
}
