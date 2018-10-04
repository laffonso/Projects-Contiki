Esta pasta contém um projeto do contiki que fica escutando a porta UART a espera de dados para encaminhar esses
dados por MQTT para um border router.

Para modificar o destino da publicação deve-se utilizar o arquivo project-conf.h. 
A maior parte do código está em mqtt-example.c
O arquivo Makefile contém as diretivas de compilação
