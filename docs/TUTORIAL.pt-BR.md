# Tutorial: servidor postmarketOS sem tela em um Redmi Note 11 Pro (MT6877)

English version: [TUTORIAL.md](TUTORIAL.md)

Este é o relato de como um Redmi Note 11 Pro (codinome `pissarro`, MediaTek MT6877,
kernel 4.14) virou um servidor Linux sempre ligado que inicia sozinho, entra no
Wi-Fi, responde SSH na rede local, protege a bateria e pode voltar ao Android sob
demanda. Tudo foi feito em **um** aparelho. Trate como relato de campo e ponto de
partida, não como procedimento suportado.

## 0. Leia antes de tudo

**Você pode inutilizar o aparelho.** O procedimento sobrescreve parte da partição
`super` e troca o `boot_b`. Exige bootloader desbloqueado. Não comece sem backups
completos e conferidos por hash e sem uma conexão `fastboot` funcionando.

**Só para o `pissarro`** (MediaTek MT6877 / Dimensity 920). Outros aparelhos vendidos
como "Redmi Note 11 Pro", como os modelos globais 4G e 5G, têm outros chips e outro
layout de partições: nada aqui se aplica a eles. A seção 3 começa conferindo o
`ro.product.device`.

O método tem riscos que você precisa entender antes de copiá-lo:

| Fato | Consequência |
|---|---|
| O rootfs fica numa **faixa de 4 GiB dentro da `super`**, não numa partição própria. | A faixa fica fora de todas as partições usadas pelo slot ativo (B), mas sobrepõe partições ainda listadas na tabela antiga do **slot A**. Não é espaço livre em nenhum sentido oficial. |
| Este aparelho é **Virtual A/B**: uma OTA grava os dados de snapshot no espaço livre da `super`. Trocar de slot inicia pela tabela do slot A. | Qualquer um dos dois pode sobrescrever a faixa e destruir o seu Linux. Desative OTA e nunca troque de slot. |
| O `boot_b` é substituído pela imagem de boot do Linux. | O Android não inicia até você restaurar o `boot_b` original (seção 11). |
| O `userdata` do Android (F2FS, criptografado) não é tocado. | Você tem só 4 GiB de rootfs. Usar o `userdata` exigiria apagar o Android de vez: fora do escopo. |
| Só o rádio Wi-Fi funciona. | Sem dados móveis, GPU, áudio, câmera, tela ou Bluetooth. Serve para um servidor sem tela. |

Nunca rebloqueie o bootloader (`fastboot flashing lock`) enquanto `boot_b` ou `super`
estiverem diferentes do original: o bootloader bloqueado recusa imagens modificadas, e
o aparelho pode não iniciar mais.

Nada aqui é endossado pela Xiaomi, MediaTek ou postmarketOS.

## 1. Arquitetura

```
 PC ── USB ──► boot_b (temporário, nosso)
               kernel (compilado do fonte público 4.14 "hydrogen") + seu DTB original
               + initramfs mínimo com um único arquivo, /init (aarch64 estático, src/initramfs)
                  │
                  ├─ gadget USB (serial ACM) ── SSH de resgate: sshd -i sobre /dev/ttyGS0
                  ├─ watchdog de boot de 600 s ─► reinicia no Fastboot se nunca confirmar
                  └─ monta em loop o rootfs ext4 de  super @ 4 GiB, tamanho 4 GiB
                       └─ switch_root → OpenRC (postmarketOS/Alpine)
                            wifi ─ nftables ─ sshd ─ térmico/carga ─ boot-confirm
                                                              │
               boot-confirm verifica Wi-Fi, IP, rota, gateway, firewall, porta SSH e
               guarda térmico, e só então dá SIGSTOP no watchdog ("este boot está bom").
```

Duas ideias de segurança sustentam tudo:

1. **Nada é confiável até ser provado.** Um boot que nunca se confirma volta sozinho
   ao Fastboot, onde você restaura o Android a partir de um PC.
2. **Toda gravação tem um caminho de volta verificado.** Antes de gravar uma faixa,
   você salva os bytes originais e confere o hash; depois de gravar, lê de volta e
   confere de novo.

## 2. O que você precisa

- Redmi Note 11 Pro (pissarro) com **bootloader desbloqueado** e root no Android
  (usamos Magisk) para ler partições e dados de calibração. Autorize o root para o
  shell do `adb` quando o Magisk perguntar.
- Uma máquina de build Linux x86_64 (VM ou WSL servem). Ferramentas: `git`, `python3`,
  `gcc-aarch64-linux-gnu`, `qemu-user-static` com binfmt aarch64, `e2fsprogs`,
  `android-tools` (`adb`, `fastboot`), `pyserial`. Cerca de 60 GB livres.
- `mkbootimg.py` e `unpack_bootimg.py` do AOSP (platform/system/tools/mkbootimg).
- Cabo USB, um roteador que você controle e um par de chaves SSH usado só para este aparelho.
- Tempo para ler cada comando antes de executá-lo.

Adapte os exemplos: a documentação usa `192.168.1.50` para o aparelho e
`192.168.1.1` para o roteador.

## 3. Faça backup de tudo primeiro

Blocos marcados **aparelho** rodam num shell root no aparelho: `adb shell` e depois
`su`. Blocos marcados **PC** rodam no seu computador. Um comando `adb` de uma linha que
precisa de root passa o comando remoto inteiro como **um** argumento entre aspas, como
em `adb exec-out "su -c '...'"`. O adb junta os argumentos sem aspas; assim, em
`adb shell su -c 'a | b'`, a parte `| b` rodaria no aparelho *sem* root, e uma gravação
ou restauração escrita desse jeito falha.

Confira de qual slot o Android roda (`ro.boot.slot_suffix`); este tutorial supõe
**`_b`**. Se o seu for `_a`, espelhe cada `_b`/`_a` abaixo e refaça a verificação da
seção 4 para o seu layout.

**Aparelho:** registre identidade e hashes. Pare se a primeira linha não for `pissarro`:

```sh
getprop ro.product.device            # pissarro
getprop ro.boot.slot_suffix          # _b
sha256sum /dev/block/by-name/boot_b /dev/block/by-name/super
blockdev --getsize64 /dev/block/by-name/super
```

**PC:** copie as duas e confira cada hash após a transferência (`adb exec-out` a partir
de Linux/macOS/WSL, nunca por um pipe de console do Windows, que corrompe dados
binários). Mantenha todos os backups **fora** do aparelho e privados:

```sh
adb exec-out "su -c 'cat /dev/block/by-name/boot_b'" > boot_b.img
adb exec-out "su -c 'cat /dev/block/by-name/super'"  > super-completa.img   # ~8,5 GiB
sha256sum boot_b.img super-completa.img   # deve ser igual à saída do aparelho acima
chmod 0444 boot_b.img super-completa.img
```

Guarde também `boot_a`, `vbmeta*` e o que mais puder. **Não** toque em `userdata`,
`persist`, `nvdata` nem nas partições do bootloader.

## 4. Escolha a faixa de armazenamento dentro da `super`

A `super` contém as partições lógicas do Android. **PC:** extraia as tabelas dos dois
slots e verifique a faixa:

```sh
adb exec-out "su -c 'lpdump --slot=1'" > lp-ativo.txt      # slot b
adb exec-out "su -c 'lpdump --slot=0'" > lp-inativo.txt    # slot a
python3 tools/check-super-range.py --active lp-ativo.txt --inactive lp-inativo.txt
```

No nosso aparelho, as partições do slot B ativo terminavam em 3,47 GiB de uma `super`
de 8,5 GiB, então `[4 GiB, 8 GiB)` não as tocava. A faixa **sobrepunha** partições
ainda listadas na tabela antiga do slot A, e por isso isto é um improviso, não um
layout. O `lpdump` também informou `virtual_ab_device`: a próxima OTA colocaria os
dados de snapshot no espaço livre da `super`, ou seja, nesta faixa. O script termina
com erro se a faixa sobrepõe o slot ativo ou passa do fim. Nunca grave uma faixa que
ele rejeite.

Salve no **PC** os bytes originais exatamente dessa faixa e depois calcule o hash da
mesma faixa no **aparelho**. Os dois hashes devem ser iguais:

```sh
# PC
adb exec-out "su -c 'dd if=/dev/block/by-name/super bs=4194304 skip=1024 count=1024 2>/dev/null'" \
  | gzip -1 > super-faixa-original.bin.gz
gzip -dc super-faixa-original.bin.gz | sha256sum
# aparelho
dd if=/dev/block/by-name/super bs=4194304 skip=1024 count=1024 2>/dev/null | sha256sum
```

(`bs=4194304 skip=1024` começa 4 GiB adiante; `count=1024` são 4 GiB de tamanho.)
Anote esse hash: a seção 11 confere a restauração contra ele.

## 5. Copie o firmware e a calibração do Wi-Fi do seu aparelho

O chip de conectividade do MT6877 precisa do firmware e de uma calibração própria de
cada unidade. **Ambos pertencem ao seu aparelho e nunca devem ser publicados**; a
calibração também contém o endereço de hardware do seu Wi-Fi. **PC:** copie do Android
(root) e mantenha em privado:

```sh
mkdir -p privado/vendor-firmware && chmod 700 privado
# firmware (8 arquivos)
for f in conninfra.cfg wifi.cfg WIFI_RAM_CODE_soc5_0_1_1.bin soc5_0_ram_mcu_1_1_hdr.bin \
         soc5_0_ram_wmmcu_1_1_hdr.bin soc5_0_ram_bt_1_1_hdr.bin BT_FW.cfg fm_cust.cfg; do
  adb exec-out "su -c 'cat /vendor/firmware/$f'" > privado/vendor-firmware/$f
done
# calibração (arquivo pequeno, no máximo 8192 bytes)
adb exec-out "su -c 'cat /mnt/vendor/nvdata/APCFG/APRDEB/WIFI'" > privado/wifi-nvram.bin
```

Crie um `wpa_supplicant.conf` para a sua rede (modo 0600, nunca versionado):

```
ctrl_interface=/run/wpa_supplicant
network={
  ssid="SUA-REDE"
  psk="SUA-SENHA"
  key_mgmt=WPA-PSK
}
```

## 6. Compile o kernel e um rootfs base

*Esta seção descreve o que fizemos; não a refizemos de ponta a ponta para este texto.
Espere precisar adaptar.*

Usamos o [pmbootstrap](https://gitlab.postmarketos.org/postmarketOS/pmbootstrap) e o
[pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports) nos commits abaixo,
em uma VM de build dedicada, com um usuário sem privilégios:

```
pmbootstrap  edb3097c7307216b088478b7c424ee07d636f41b   (v3.11.1, instalado num venv Python)
pmaports     972f578fc9e87831adc4b3c7ba4c0d66461f2060
```

Copie `port/device-xiaomi-pissarro` e `port/linux-xiaomi-pissarro` para
`device/downstream/` desse checkout do pmaports e aponte o pmbootstrap para ele com
`-p` (senão ele usa o próprio clone). O `pmbootstrap init` faz as mesmas perguntas de
forma interativa; este é o nosso equivalente não interativo, seguido do build e da
instalação do rootfs:

```sh
pmb="pmbootstrap -p /caminho/do/pmaports"
$pmb config device xiaomi-pissarro
$pmb config ui console
$pmb config service_manager openrc
$pmb config extra_packages openssh,wpa_supplicant,nftables
$pmb config hostname meu-servidor    # opcional; usuário e fuso também são escolha sua
$pmb build linux-xiaomi-pissarro
$pmb build device-xiaomi-pissarro
# a senha é do usuário padrão do pmbootstrap; o configure-rootfs.sh a bloqueia
$pmb install --no-image --no-recommends --password "$(openssl rand -hex 16)"
$pmb shutdown                        # desmonta tudo dentro dos chroots
```

Notas do build do kernel:

- O kernel é a árvore pública "hydrogen" 4.14.356 do MT6877 citada em
  `port/linux-xiaomi-pissarro/APKBUILD` (commit fixado), compilada com Clang/LLVM
  (usamos LLVM 23.1.2 e DTC 1.7.2).
- Correções necessárias: cabeçalhos do kernel para as ferramentas SELinux do host,
  remoção do VDSO de compatibilidade de 32 bits e uso do `dtc` do host no lugar do
  binário glibc pré-compilado do fabricante (`0001-use-host-dtc.patch`).
- `deviceinfo_flash_method="none"` é proposital: o pmbootstrap nunca deve gravar
  neste aparelho.
- O pacote de dispositivo é só um veículo para o kernel. **Não** usamos a imagem de
  boot nem o flasher do pmbootstrap.

Extraia o kernel do pacote (o `.apk` é uma concatenação de fluxos tar gzip):

```sh
# o pacote fica no diretório de trabalho do pmbootstrap, em packages/edge/aarch64/
tar --ignore-zeros -xzf linux-xiaomi-pissarro-4.14.356-r0.apk boot/vmlinuz
mv boot/vmlinuz Image.gz
```

O `pmbootstrap install --no-image` deixa o rootfs como árvore de diretórios no
diretório de trabalho (`chroot_rootfs_xiaomi-pissarro`): uma árvore Alpine/OpenRC
aarch64 simples, com cerca de 560 MB e sem gerenciador de tela. Copie para um lugar
seu, preservando donos, modos, ACLs e atributos estendidos:

```sh
sudo mkdir copia-do-rootfs
sudo tar --one-file-system --acls --xattrs --numeric-owner -cpf - \
     -C /caminho/do/work/chroot_rootfs_xiaomi-pissarro . \
  | sudo tar --acls --xattrs --numeric-owner -xpf - -C copia-do-rootfs
```

## 7. Compile os utilitários e a imagem de boot

```sh
sh tools/build-helpers.sh          # -> out/init, wifi-init, redmi-thermal-daemon, redmi-reboot-bootloader
python3 tools/build-boot-image.py \
    --original boot_b.img --kernel Image.gz --init out/init \
    --mkbootimg caminho/para/mkbootimg.py --out boot-linux.img
```

O `build-boot-image.py` lê o layout, o DTB, a linha de comando e o rodapé AVB do
**seu** `boot_b.img`. Ele remove `root=`, `rdinit=`, `init=`, `panic=`,
`skip_initramfs` e `androidboot.force_normal_boot=` da sua linha de comando e
acrescenta `root=/dev/ram rdinit=/init panic=10 selinux=0`. Nunca imprime a linha de
comando (ela pode conter identificadores do aparelho). O rodapé AVB da imagem original
é mantido para o bootloader encontrar a estrutura que espera; o hash interno deixa de
bater, o que só funciona com o bootloader **desbloqueado**.

*Verificado:* no nosso aparelho, este script reproduziu a imagem de boot instalada
byte a byte (mesmo SHA-256) a partir do `boot_b` original, do `Image.gz` compilado e do
`init` instalado. O `init` compilado hoje a partir de `src/initramfs` difere daquele só
na string de marcação dos logs, então a sua imagem não terá o mesmo hash.

Confira o resultado antes de gravar qualquer coisa:

```sh
python3 unpack_bootimg.py --boot_img boot-linux.img --out desempacotado   # header v2, tamanhos esperados
sha256sum boot-linux.img && ls -l boot-linux.img boot_b.img               # mesmo tamanho
```

## 8. Configure o rootfs e crie a imagem ext4

```sh
sudo ROOTFS=/caminho/da/copia-do-rootfs \
     DEVICE_IP=192.168.1.50 GATEWAY_IP=192.168.1.1 LAN_CIDR=192.168.1.0/24 PREFIX_LEN=24 \
     SSH_PUBLIC_KEY=~/.ssh/redmi_ed25519.pub \
     WPA_CONF=privado/wpa_supplicant.conf WIFI_NVRAM=privado/wifi-nvram.bin \
     VENDOR_FIRMWARE_DIR=privado/vendor-firmware HELPERS_DIR=out \
     sh tools/configure-rootfs.sh
```

Ele instala os utilitários e serviços, configura SSH somente por chave, gera o firewall
para a sua rede, desativa os serviços de rede concorrentes e ativa os nossos. Copia
apenas a sua chave **pública**; a chave de host do SSH é gerada dentro do rootfs.

*Verificado:* executado contra uma cópia nova do rootfs do pmbootstrap, terminou sem
erros, produziu o runlevel padrão esperado e os mesmos binários (hashes) que rodam no
aparelho. Leia o script antes de executá-lo.

Crie a imagem. Os recursos do ext4 importam: o `orphan_file`, padrão desde o
e2fsprogs 1.47, exige kernel 5.15 ou mais novo, então precisa ficar desligado neste
kernel 4.14. Desligamos também o `metadata_csum_seed`, o outro padrão novo, para manter
o conjunto de recursos que testamos. Com e2fsprogs anterior ao 1.47, retire o
`^orphan_file` (essas versões não conhecem o recurso e recusam a opção):

```sh
truncate -s 4G rootfs.img
mkfs.ext4 -F -L redmi-linux -m 0 -O ^orphan_file,^metadata_csum_seed \
          -E lazy_itable_init=0,lazy_journal_init=0 rootfs.img
sudo mount -o loop rootfs.img /mnt/rootfs
sudo tar --one-file-system --acls --xattrs --numeric-owner -cpf - -C /caminho/da/copia-do-rootfs . \
  | sudo tar --acls --xattrs --numeric-owner -xpf - -C /mnt/rootfs
sudo umount /mnt/rootfs
e2fsck -fn rootfs.img && gzip -1 -k rootfs.img && sha256sum rootfs.img rootfs.img.gz
```

Mantenha `rootfs.img` com exatamente 4 GiB: o initramfs confere o número mágico do
ext4 no byte `4 GiB + 1080` da `super` e recusa montar qualquer outra coisa.

## 9. Grave a imagem na faixa da super

Só faça isto depois de concluir e verificar as seções 3 e 4.

1. Bateria abaixo de cerca de 38 °C e Android ocioso. Gravar 4 GiB esquenta o aparelho;
   nós nos recusamos a começar acima de 40,0 °C e paramos em 41,0 °C
   (`/sys/class/power_supply/battery/temp` é em décimos de grau).
2. **PC:** coloque a imagem comprimida no aparelho. **Aparelho:** confira lá:

   ```sh
   adb push rootfs.img.gz /data/local/tmp/          # PC
   sha256sum /data/local/tmp/rootfs.img.gz          # aparelho: deve bater com o do PC
   ```

3. **Aparelho:** grave. Os dois primeiros testes param antes de qualquer gravação se a
   `super` estiver somente leitura ou for pequena demais para a faixa;
   `bs=4194304 seek=1024` é exatamente 4 GiB adiante na `super`:

   ```sh
   set -o pipefail
   [ "$(blockdev --getro /dev/block/by-name/super)" = 0 ] &&
   [ "$(blockdev --getsize64 /dev/block/by-name/super)" -ge 8589934592 ] &&
   gzip -dc /data/local/tmp/rootfs.img.gz |
     dd of=/dev/block/by-name/super bs=4194304 seek=1024 conv=notrunc,fsync &&
   sync && echo WRITE_OK
   ```

4. **Aparelho:** leia de volta e compare com o SHA-256 do `rootfs.img` no PC:

   ```sh
   dd if=/dev/block/by-name/super bs=4194304 skip=1024 count=1024 2>/dev/null | sha256sum
   ```

   Se for diferente, restaure a faixa original imediatamente (seção 11) e pare. Se
   bater, apague o `/data/local/tmp/rootfs.img.gz`.

Nós enviamos a imagem por SSH em vez de `adb push`, com um controle de temperatura a
cada 30 s, mas a gravação em si é o mesmo `gzip -dc | dd`, rodando como root depois do
mesmo tipo de verificação de somente leitura e de tamanho.

## 10. Primeiro boot

```sh
adb reboot bootloader
fastboot getvar product          # pissarro
fastboot getvar current-slot     # b
fastboot getvar unlocked         # yes
fastboot flash boot_b boot-linux.img
fastboot reboot
```

(O `fastboot boot` falhou no nosso aparelho com `usb_read failed (31)`, então gravamos o
`boot_b`, e por isso o original precisa ser restaurado depois.)

Aparece um dispositivo serial USB (vendor ID `1d6b`, product `0104`). O init imprime
linhas `BOOT_STAGE=...` nele. Abra uma sessão SSH de resgate por ele:

```sh
ssh -o "ProxyCommand=python3 tools/serial-ssh-proxy.py /dev/ttyACM0" \
    -o HostKeyAlias=redmi-linux-rootfs root@resgate
```

(No Windows use a porta COM, por exemplo `COM5`. Coloque a chave de host SSH do rootfs,
de `/etc/ssh/ssh_host_ed25519_key.pub` dentro da sua imagem, no seu `known_hosts` com o
alias `redmi-linux-rootfs`, e use `StrictHostKeyChecking=yes`.)

**Você tem 600 segundos.** Se o `redmi-boot-confirm` não provar que o servidor está
saudável até lá, o watchdog reinicia no Fastboot. Quando Wi-Fi, endereço, rota, ping ao
gateway, firewall, porta SSH e guarda térmico estiverem todos bons, o serviço pausa o
watchdog e cria `/run/redmi-boot-confirmed`. Daí em diante o aparelho fica ligado e o
SSH funciona pela rede local: `ssh -p 2222 root@192.168.1.50`.

Se o cabo USB for reconectado e o PC deixar de ver a porta serial, refaça a ligação do
gadget a partir do aparelho: `echo "" > /sys/kernel/config/usb_gadget/redmi_probe/UDC;
echo musb-hdrc > /sys/kernel/config/usb_gadget/redmi_probe/UDC`.

Depois rode `tools/reboot-cycle-test.sh 3` e deixe o `tools/soak-monitor.sh` observar por
24 horas antes de confiar nele. Os dois scripts leem `DEVICE_IP`, `KNOWN_HOSTS` (um
arquivo só com a chave de host do rootfs) e `SSH_KEY` do ambiente; veja o cabeçalho de
cada um.

## 11. Recuperação: voltar ao Android

**A partir do Linux em execução**, por SSH (qualquer um funciona; ambos levam o
aparelho ao Fastboot):

```sh
/usr/local/sbin/redmi-reboot-bootloader --confirm-bootloader
# ou, se não estiver disponível, acione o watchdog pausado:
kill -USR1 "$(cat /run/redmi-watchdog.pid)"; kill -CONT "$(cat /run/redmi-watchdog.pid)"
```

**PC, no Fastboot:** confirme que é o aparelho e o slot certos e devolva o `boot_b`
original:

```sh
fastboot getvar product          # pissarro
fastboot getvar current-slot     # b
fastboot flash boot_b boot_b.img # o seu ORIGINAL, com hash conferido
fastboot reboot
```

O Android volta a iniciar. Devolva os bytes originais à faixa da `super`, para que os
dados do próprio Android fiquem íntegros (a faixa sobrepunha a tabela antiga do
slot A). **PC:** `adb push super-faixa-original.bin.gz /data/local/tmp/`; depois, no
**aparelho**:

```sh
sha256sum /dev/block/by-name/boot_b          # deve ser igual ao hash da seção 3
set -o pipefail
[ "$(blockdev --getro /dev/block/by-name/super)" = 0 ] &&
gzip -dc /data/local/tmp/super-faixa-original.bin.gz |
  dd of=/dev/block/by-name/super bs=4194304 seek=1024 conv=notrunc,fsync &&
sync && echo RESTORE_OK
dd if=/dev/block/by-name/super bs=4194304 skip=1024 count=1024 2>/dev/null | sha256sum
```

O último hash deve ser igual ao que você anotou na seção 4. No nosso aparelho, a volta
completa, a partir do Linux em execução, levou 3 minutos e 18 segundos, com `boot_b` e
faixa conferidos por hash. Se não conseguir chegar ao Fastboot de jeito nenhum, segure
Volume − e Power para entrar nele; o bootloader em si nunca foi modificado.

## 12. O que cada serviço faz

| Serviço | Função |
|---|---|
| `redmi-privacy` | Apaga o backlight e remove os nós de dispositivo da câmera, da captura de microfone e dos rádios sem uso (Bluetooth, GPS, infravermelho, digital) a cada boot. **Não** grava nos LEDs de flash/lanterna: o driver mt6360 dispara o flash a qualquer gravação de brilho, mesmo `0`. |
| `redmi-thermal` (`redmi-thermal-daemon`) | A cada 10 s lê as temperaturas da bateria, do carregador e da CPU. Mantém a bateria numa **janela de carga de 60–80 %** alternando `input_suspend`; suspende a carga com calor (bateria: leve 42 °C, forte 45 °C), falha fechado com sensores inválidos, desliga a 55 °C sustentados e limita a frequência da CPU pelo PPM da MediaTek. |
| `redmi-wifi` | Roda o `wifi-init` (abre o driver de conectividade, entrega a sua calibração, habilita a interface station), depois o `wpa_supplicant`, e então define IP fixo e rota. |
| `redmi-wifi-guard` | Recuperação escalonada quando a associação se perde: busca em ~1 min, reassociação em ~2 min e reinício só do `wpa_supplicant` em ~5 min. |
| `nftables` | Entrada com descarte por padrão; permite loopback, tráfego estabelecido e SSH + ICMP apenas da sua rede local. IPv6 descartado. |
| `sshd` | Porta 2222, root só com chave, sem encaminhamento, aguarda firewall e Wi-Fi. |
| `redmi-boot-confirm` | Verifica toda a pilha e então pausa o watchdog de boot. |
| `redmi-soak-logger` | Uma linha de saúde a cada 10 minutos em `/var/lib/redmi-soak/health.log`. |
| `chronyd` | Sincronismo de hora (precisa de `rc_provide="net"` no `redmi-wifi`, veja abaixo). |

## 13. O que deu errado (e as correções)

- **`wpa_supplicant` reiniciando para sempre.** O `wpa_supplicant` 2.11-r4 do Alpine é
  compilado sem `CONFIG_DEBUG_FILE`. Passar `-f /dev/null` fazia ele imprimir o uso e sair
  com status 0 antes de criar o socket de controle, e o `supervise-daemon` o reiniciava
  em laço. Não passe `-f`. Note também que o pidfile do `supervise-daemon` guarda o PID do
  supervisor, não o do daemon.
- **Firewall no kernel 4.14.** O kernel não tem `NFT_META` nem backends de set do
  nf_tables. As regras só podem usar expressões payload, cmp, bitwise e ct: uma tabela
  `ip` (não `inet`), `ip protocol tcp` antes de cada porta, uma regra por porta (sem sets)
  e loopback por endereço (`ip saddr 127.0.0.0/8`), não por `iifname`. Habilitar
  `CONFIG_NFT_META` etc. no kernel removeria essa limitação.
- **Wi-Fi preso em `SCANNING` por 5 horas** depois de o roteador derrubar o enlace à
  noite, com a rede visível a -40 dBm; um `wpa_cli scan` manual resolveu. A causa não
  pôde ser provada (a saída do supplicant é descartada e o log do kernel é inundado por
  uma mensagem repetida de um driver térmico). O `redmi-wifi-guard` é a mitigação; foi
  testado forçando uma desconexão (recuperou em cerca de dois minutos) e encerrando o
  supplicant (recuperou em cerca de 9 segundos).
- **O `chronyd` nunca iniciava.** Ele exige `net`, e o único provedor era o serviço
  `networking` que tínhamos desativado, que então falhava. `rc_provide="net"` em
  `/etc/conf.d/redmi-wifi` resolve. Garanta também que rode **um só** `logbookd`: uma
  segunda cópia assumiu o `/dev/log` e travou o `chronyd` num socket cheio.
- **O flash da câmera piscou** quando um serviço gravou `0` nos LEDs
  `flash-light*`/`torch-light*`. O driver pulsa o flash a cada gravação. Não mexa nesses
  LEDs. Dois pulsos por volta de 2 s de cada boot vêm da inicialização do driver no
  kernel e não podem ser evitados pelo espaço de usuário.
- **Load average perto de 20** é cosmético: cerca de 20 threads de kernel da MediaTek
  ficam em espera ininterruptível (`D`) enquanto a CPU está ~99 % ociosa.
- **`scaling_cur_freq` mente.** É um índice mantido pelo driver; a frequência real do PLL
  era diferente, e nem `scaling_max_freq` continha o cluster grande. O limite do PPM da
  MediaTek (`/proc/ppm/policy/hard_userlimit_max_cpu_freq`) continha.
- **O OpenSSH recusou logins por chave da conta root** enquanto o campo de senha estava
  travado (`!`). O `configure-rootfs.sh` troca o campo por `NP`, que nenhuma senha
  consegue satisfazer e que o OpenSSH não trata como conta travada; senha e
  keyboard-interactive continuam desativados.
- **Windows e serial USB.** O Windows descarta a entrada serial quando uma porta COM é
  aberta, então o ouvinte de resgate espera uma linha `STARTSSH` e só então responde
  `BOOTSTRAP_READY` e inicia o `sshd -i`. Use o `serial-ssh-proxy.py`, que faz esse
  handshake.

## 14. Lista de verificação

- [ ] Boot a frio, sem intervenção, chega a `redmi-boot-confirmed` em cerca de 30 s.
- [ ] Três reinícios seguidos, cada um com boot ID novo (`tools/reboot-cycle-test.sh 3`).
- [ ] O Wi-Fi reconecta após reiniciar o roteador; o log do guard mostra o que ele fez.
- [ ] De outro computador da rede, só SSH e ping respondem; de fora da rede, nada.
- [ ] A carga pausa em 80 % e volta em 60 %; as temperaturas ficam na faixa.
- [ ] Retorno ao Android ensaiado uma vez **antes** de você depender do servidor.
- [ ] 24 horas de `tools/soak-monitor.sh` sem reinício inesperado.

## 15. Limitações conhecidas

- Rootfs de 4 GiB, numa faixa que a OTA do Android pode sobrescrever (o Virtual A/B
  guarda os snapshots da atualização no espaço livre da `super`).
- Exige bootloader desbloqueado e root no Android; a verificação de boot é contornada.
- Kernel 4.14 com drivers do fabricante; sem suporte no kernel principal.
- Testado em uma unidade e uma ROM. Layouts de partição e caminhos de calibração podem variar.
- O modem fica como o bootloader o carregou; não há jeito seguro de pará-lo sem o
  espaço de usuário do Android.

## Privacidade: o que nunca vai para um repositório público

Rode `python3 tools/audit-privacy.py` antes de cada commit. Não publique: imagens de
disco ou de boot, firmware, a calibração do Wi-Fi, o `wpa_supplicant.conf`, chaves SSH ou
`known_hosts`, logs, seus endereços, nem nada lido do `userdata`. Use o e-mail de
no-reply do GitHub nos commits (`ID+USUARIO@users.noreply.github.com`) para que o seu
e-mail real não fique gravado no histórico.
