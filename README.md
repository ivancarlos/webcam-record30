# Gravador

Gravador completo de vídeo da webcam que combina **V4L2 + FFmpeg** para gravar
30 segundos de vídeo em alta qualidade!

## **Principais características:**

### **Captura (V4L2):**
- Interface direta com o kernel Linux
- Buffers mapeados em memória para eficiência
- Resolução 640x480 @ 25 FPS
- Formato YUYV (padrão das webcams)

### **Codificação (FFmpeg):**
- Codec **H.264** (alta compressão)
- Saída em **MP4** (compatibilidade universal)
- Conversão otimizada YUYV → YUV420P
- Bitrate 2 Mbps (boa qualidade)
- Preset "fast" para codificação em tempo real

### **Controles:**
- **30 segundos** de gravação automática
- **Ctrl+C** para parar antecipadamente
- Progresso em tempo real
- Estatísticas finais

## **Como usar:**

### **1. Instalar dependências:**
```bash
sudo apt update
sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev v4l-utils
```

### **2. Compilar:**
```bash
make
```

### **3. Testar webcam:**
```bash
make test-webcam
```

### **4. Gravar vídeo:**
```bash
# Gravação simples (30s)
make record
# ou
./webcam_video_recorder

# Especificar dispositivo e arquivo
./webcam_video_recorder /dev/video1 meu_video.mp4
```

## **Exemplo de uso:**
```bash
$ ./webcam_video_recorder
=== Gravador de Vídeo Webcam ===
Dispositivo: /dev/video0
Arquivo de saída: webcam_video.mp4
Duração: 30 segundos
FPS: 25

Webcam: USB Camera (uvcvideo)
Resolução: 640x480
Encoder inicializado: webcam_video.mp4 (640x480 @ 25 fps)
Iniciando gravação... (Ctrl+C para parar)
Gravado: 1/30 segundos (25 frames)
Gravado: 2/30 segundos (50 frames)
...
Gravado: 30/30 segundos (750 frames)

Finalizando gravação...
Gravação finalizada: 750 frames escritos

=== Gravação Concluída ===
Arquivo: webcam_video.mp4
Frames gravados: 750
Duração real: 30.1 segundos
FPS médio: 24.9
```

## **Recursos técnicos avançados:**

- **Conversão de cor otimizada**: YUYV → YUV420P com subsample 4:2:0
- **Controle de taxa**: Sincronização para manter 25 FPS
- **Gerenciamento de memória**: Buffers reutilizáveis, sem vazamentos
- **Tratamento de sinais**: Finalização

