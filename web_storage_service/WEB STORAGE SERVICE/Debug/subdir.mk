################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CC_SRCS += \
../wbs.cc 

CC_DEPS += \
./wbs.d 

OBJS += \
./wbs.o 


# Each subdirectory must supply rules for building sources it contributes
%.o: ../%.cc subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: Cross G++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean--2e-

clean--2e-:
	-$(RM) ./wbs.d ./wbs.o

.PHONY: clean--2e-

