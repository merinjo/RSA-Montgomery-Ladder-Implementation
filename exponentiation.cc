#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>

#define SIZE 256
#define CL_SIZE 64
#define PAGE_SHIFT 12
#define PAGEMAP_LENGTH 8
#define ORIG_BUFFER "Hello, World!"
#define NEW_BUFFER "Hello, Linux!"

using namespace std;

struct timeval start;
struct timeval end;
double diff = 0;
unsigned char base[SIZE*2] __attribute__((aligned(64)));
unsigned char result[SIZE] __attribute__((aligned(64)));
void* create_buffer(void);
unsigned long get_page_frame_number_of_address(void *addr);
int open_memory(void);
void seek_memory(int fd, unsigned long offset);

// Function Definitions
// adding test comments
inline void clflush(volatile void *p, unsigned int size)
{
   for(unsigned int i=0; i<size; i+=CL_SIZE)
   {
      asm volatile ("clflush (%0)" :: "r"(p+i));
   }
}

void load_file(const char* file_name, unsigned char* buffer)
{
   *buffer = 0;
   long int length;
   FILE* file = fopen(file_name, "rb");

   if (file)
   {
      fseek (file, 0, SEEK_END);
      length = ftell (file);
      fseek (file, 0, SEEK_SET);
      if (buffer)
      {
         fread (buffer, sizeof(unsigned char), length, file);
      }
      else
      {
         printf("ERROR: Unable to open file.\n");
         exit(1);
      }
      // Reversing the array
      for(int j = 0; j < length/2; j++)
      {
         unsigned char c = buffer[j];
         buffer[j] = buffer[(length-1) - j];
         buffer[length - j - 1] = c;
      }
   }
   fclose (file);
}


void shift_exp (unsigned char* x)
{
   unsigned char array_to_shift[SIZE];

   memset(array_to_shift, 0x00, (SIZE));

   for ( int i = 0; i < (SIZE/2); i++)
   {
      array_to_shift[i] = x[i];
   }

   unsigned char msb = 0x00;

   for (int i = SIZE/2-1; i > 0; i--)
   {
      array_to_shift[i] = array_to_shift[i] << 1;

      if ((array_to_shift[i-1] & 0x80))		//Checking the msb of (i)th byte
      {
         array_to_shift[i]++;
      }
   }
   array_to_shift[0] = array_to_shift[0] << 1;

   for ( int i = 0; i < SIZE/2 ; i++)
   {
      x[i] = array_to_shift[i];
   }

   return;
}

void shift_base (unsigned char* x)
{
   unsigned char array_to_shift[SIZE];

   memset(array_to_shift, 0x00, SIZE);

   std::copy(&x[0], &x[SIZE-1], array_to_shift);

   unsigned char msb = 0x00;

   for (int i = SIZE/2-1; i > 0; i--)
   {
      array_to_shift[i] = array_to_shift[i] << 1;

      if ((array_to_shift[i-1] & 0x80))		//Checking the msb of (i)th byte
      {
         array_to_shift[i]++;    // Shifting the msb of (i)th byte to lsb of (i+1)th byte
      }
   }

   array_to_shift[0] = array_to_shift[0] << 1;

   std::copy(&array_to_shift[0], &array_to_shift[(SIZE)-1], x);

   return;
}

void add (unsigned char* result, unsigned char* x)
{
   char carry = 0x00;
   unsigned int temp = 0;

   for (int i = 0; i < (SIZE); i++)
   {
      if (carry == 0x00)
      {
         temp = (unsigned int) (result[i] + x[i]);
         result[i] = result[i] + x[i];
      }
      else
      {
         temp = (unsigned int) (result[i] + x[i] + carry);
         result[i] = result[i] + x[i] + carry;
      }

      // Check For Non-Zero Carry

      if (temp > 256)	//The result byte cannot store a value greater then 255 or 0xFF
      {
         result[i] = (result[i] & 0xFF); // To store the 8 bits in result
         carry = 0x01;
      }
      else
      {
         carry = 0x00;
      }
   }

   return;
}

void multiply_add (unsigned char* result, unsigned char* x, unsigned char* y)
{
   // Temporay Arrays to avoid changes in original ones

   unsigned char array_r[SIZE];
   unsigned char array_x[SIZE];

   memset(array_x, 0x00, SIZE);
   memset(array_r, 0x00, SIZE);

   for ( int i = 0; i < SIZE; i++)
   {
      array_x[i] = x[i];
   }

   int i = 0;
   int end = 0; // To keep track of the last byte for Y

   const unsigned int mask_size = 8; // Masks to check the LSB of Y or Multiplier
   const unsigned char masks[mask_size] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

   while (end < SIZE)
   {
      for ( int j = 0; j < mask_size; j++)
      {
         if ((y[i] & masks[j]))	// To check Y's LSB is one or not
         {
            add(array_r, array_x);
         }
         shift_base(array_x);  //In order to left shift X
      }
      i++ ;
      end++ ;
   }

   std::copy(&array_r[0], &array_r[SIZE-1], result);

   return;
}

void* create_buffer(void) {
   size_t buf_size = strlen(ORIG_BUFFER) + 1;

   // Allocate some memory to manipulate
   char *buffer = (char*) malloc(buf_size); // It will go to HEAP
   if(buffer == NULL) {
      fprintf(stderr, "Failed to allocate memory for buffer\n");
      exit(1);
   }

   // Lock the page in memory
   // Do this before writing data to the buffer so that any copy-on-write
   // mechanisms will give us our own page locked in memory
   if(mlock(buffer, buf_size) == -1) {
      fprintf(stderr, "Failed to lock page in memory: %s\n", strerror(errno));
      exit(1);
   }

   // Add some data to the memory
   strncpy(buffer, ORIG_BUFFER, strlen(ORIG_BUFFER));

   return buffer;
}

unsigned long get_page_frame_number_of_address(void *addr) {
   // Open the pagemap file for the current process
   FILE *pagemap = fopen("../../../../proc/self/pagemap", "rb");

   // Seek to the page that the buffer is on
   unsigned long offset = (unsigned long)addr / getpagesize() * PAGEMAP_LENGTH;
   if(fseek(pagemap, (unsigned long)offset, SEEK_SET) != 0) {
      fprintf(stderr, "Failed to seek pagemap to proper location\n");
      exit(1);
   }

   // The page frame number is in bits 0-54 so read the first 7 bytes and clear the 55th bit
   unsigned long page_frame_number = 0;
   fread(&page_frame_number, 1, PAGEMAP_LENGTH-1, pagemap);

   page_frame_number &= 0x7FFFFFFFFFFFFF;

   fclose(pagemap);

   return page_frame_number;
}

int open_memory(void) {
   // Open the memory (must be root for this)
   int fd = open("/dev/mem", O_RDWR);

   if(fd == -1) {
      fprintf(stderr, "Error opening /dev/mem: %s\n", strerror(errno));
      exit(1);
   }

   return fd;
}

void seek_memory(int fd, unsigned long offset) {
   unsigned pos = lseek(fd, offset, SEEK_SET);

   if(pos == -1) {
      fprintf(stderr, "Failed to seek /dev/mem: %s\n", strerror(errno));
      exit(1);
   }
}


int main()
{
   unsigned char *exp;
   unsigned char *backup_exp;
   long long int counter;

   struct timeval start;
   struct timeval end;
   double diff = 0;

   // Memory Allocations

   posix_memalign((void**) &exp, 64, 256 * sizeof(char));
   posix_memalign((void**) &backup_exp, 64, 128 * sizeof(char));

   // Array Initializations

   memset(backup_exp, 0x00, 128);    // 128 Bytes or 1024 Bits fixed for Exponent
   load_file("exp.txt", backup_exp);

   // Printing the given input of Base and Exponent
   int cntr = 0;
   /*
      printf("base is: ");
      for ( int i = (SIZE-1); i > 0; i--)
      {
      if (base[i] == 0x00 && base[i-1] != 0x00)
      {
      cntr++;
      }

      if (cntr == 1)
      {
      printf("%x ", base[i-1]);
      }
      }
      printf("\n");
      */
   /*cntr = 0;
   printf("exp is: ");
   for ( int i = (SIZE - 1); i > 0; i--)
   {
      if (backup_exp[i] == 0x00 && backup_exp[i-1] != 0x00)
      {
         cntr++;
      }

      if ( cntr == 1)
      {
         printf("%x ", backup_exp[i-1]);
      }
   }
   printf("\n");*/


   int file_num = 1;
   cout << "Base address : " << &base << endl;
   cout << "Result address : " << &result << endl;

   printf("\nPrinting Page Frame Number Information:\n");
   unsigned int page_frame_number_result = get_page_frame_number_of_address(result);
   printf("Page frame for the result: 0x%x\n", page_frame_number_result);
   unsigned int distance_from_page_boundary_result = (unsigned long)result % getpagesize();
   uint64_t offset_result = (page_frame_number_result << PAGE_SHIFT) + distance_from_page_boundary_result;
   int mem_fd_result = open_memory();
   seek_memory(mem_fd_result, offset_result);
   unsigned physical_addr_result = (page_frame_number_result << PAGE_SHIFT) + distance_from_page_boundary_result;
   printf("\nPrinting Result Information:\n");
   printf("Virtual Address is: %p\n", (void*)(result));
   printf("Physical Address is: %p\n", (void*)(physical_addr_result));
   close(mem_fd_result);

   printf("\nPrinting Page Frame Number Information:\n");
   unsigned int page_frame_number = get_page_frame_number_of_address(base);
   printf("Page frame for the base: 0x%x\n", page_frame_number);
   unsigned int distance_from_page_boundary = (unsigned long)base % getpagesize();
   uint64_t offset = (page_frame_number << PAGE_SHIFT) + distance_from_page_boundary;
   int mem_fd = open_memory();
   seek_memory(mem_fd, offset);
   unsigned physical_addr = (page_frame_number << PAGE_SHIFT) + distance_from_page_boundary;
   printf("\nPrinting Base Information:\n");
   printf("Virtual Address is: %p\n", (void*)(base));
   printf("Physical Address is: %p\n", (void*)(physical_addr));
   close(mem_fd);

   int n = 0;
   /////////////////////////////////////////////////// Implementation of Montgomery Ladder //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

   while(1)
   {
      counter = 0;
      memset(base, 0x00, SIZE*2);  // Base should also be 1024 Bits or 128 Bytes
      memset(result, 0x00, SIZE);
      memset(exp, 0x00, SIZE);

      switch(file_num)
      {
         case 1:
            load_file("input1.txt", base);
            std::copy(&backup_exp[0], &backup_exp[127], exp);
            break;

         case 2:
            load_file("input2.txt", base);
            std::copy(&backup_exp[0], &backup_exp[127], exp);
            break;
         case 3:
            load_file("input4.txt", base);
            std::copy(&backup_exp[0], &backup_exp[127], exp);
            break;
         default:
            load_file("input1.txt", base);
            std::copy(&backup_exp[0], &backup_exp[127], exp);
            break;

      }
      result[0] = 0x01;
      gettimeofday(&start, NULL);
      int f =0;
      while (counter < ((SIZE/2)*8))
      {
         bool check  = (exp[(SIZE/2)-1] & 0x80)? true : false; // checking the MSB of exponent

         if(check == true) // bit is 1
         {
            f = 1;
            multiply_add(result, base, result); // res = base*res
            clflush((void *)&result, SIZE);
            clflush((void *)&base, SIZE*2);

            multiply_add(base, base, base); // base = base^2
            clflush((void *)&base, SIZE*2);
            clflush((void *)&result, SIZE);
         }

         else // bit is 0
         {
            multiply_add(base, result, base); // base = res * base
            clflush((void *)&base, SIZE*2);
            clflush((void *)&result, SIZE);

            multiply_add(result, result, result); // res = res^2
            clflush((void *)&result, SIZE);
            clflush((void *)&base, SIZE*2);
         }

         shift_exp(exp);   //exp = exp << 1
         counter++;

      }

      gettimeofday(&end, NULL);
      diff += ((double) (end.tv_usec - start.tv_usec) / 1000000 + (double) (end.tv_sec - start.tv_sec));
      printf("Total Time = %f\n", diff);

      cout << "file number: " << n << endl;
      // Printing the final result of exponentiation
      int num = 0, cntr =0;

      printf("result is: ");
        for ( int i = (SIZE-1); i > 0; i--)
        {
        if (result[i] == 0x00 && result[i-1] != 0x00)
        {
        cntr++;
        }

        if (cntr == 1)
        {
        printf("%x ", result[i-1]);
        num++;
        }
        }
        printf("\n");

      file_num++;
      if(file_num > 3)
      {
         file_num = 1;
      }
      n++;
      multiply_add(result, result, result); // res = base*res
      multiply_add(base, base, base); // base = base^2
   }

   //////////////////////////////////////////////////////////////////////END/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

   return 0;
}
