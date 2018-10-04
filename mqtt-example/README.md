Esta pasta contém um exemplo de comunicação utilizando MQTT que foi modificado para receber dados via UART e encaminhar estes
dados para um border router utilizando MQTT.

o arquivo project-conf.h possui o endereço para o qual os dados serão encaminhados enquanto o arquivo mqtt-example.c possui a 
maior parte do código. o Arquivo Makefile possui todas as diretivas de compilação e para que seja compilado com sucesso, o Makefile deve apontar onde está a pasta do contiki para encontrar os arquivos necessários para compilação.


