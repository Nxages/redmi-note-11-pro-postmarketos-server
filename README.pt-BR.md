# Redmi Note 11 Pro (MT6877) como servidor postmarketOS sem tela

Transforme um Xiaomi **Redmi Note 11 Pro** (codinome `pissarro`, MediaTek MT6877,
kernel 4.14) num servidor Linux sempre ligado: ele inicia sozinho, entra no Wi-Fi com
endereço fixo, responde SSH somente por chave na rede local atrás de um firewall,
mantém a bateria entre 60 % e 80 % e pode voltar ao Android sob demanda.

English: [README.md](README.md)

> **Aviso.** Isto sobrescreve parte da partição `super` e troca o `boot_b`. Exige
> bootloader desbloqueado e pode inutilizar o aparelho. A faixa de armazenamento usada
> sobrepõe o slot inativo do Android, então **uma atualização OTA ou a troca de slot
> pode destruir o seu Linux**. Leia a seção 0 de [docs/TUTORIAL.pt-BR.md](docs/TUTORIAL.pt-BR.md)
> antes de qualquer coisa. É um relato de campo de uma unidade, não um procedimento
> suportado. Sem vínculo com Xiaomi, MediaTek ou postmarketOS.

## O que foi verificado na unidade de referência

| Item | Resultado |
|---|---|
| Boot independente a partir de um `boot_b` temporário, rootfs numa faixa da `super` | funciona; saudável cerca de 30 s após o boot |
| Associação Wi-Fi, IP fixo, reconexão | funciona; uma queda de 5 h após o roteador derrubar o enlace foi mitigada por um serviço vigia |
| SSH só por chave na LAN, firewall com descarte padrão (limites do kernel 4.14) | funciona |
| SSH de resgate por serial USB, watchdog de boot de 600 s | funciona |
| Volta ao Android a partir do Linux em execução | ensaiada, 3 min 18 s, `boot_b` e faixa restaurados com hash conferido |
| Ciclos de reinício | 5 de 5 aprovados |
| Janela de carga 60–80 %, proteção térmica | funcionando |
| Nós de tela, câmera e microfone desativados | funcionando |
| Imagem de boot reproduzível a partir dos fontes daqui | idêntica byte a byte à instalada |
| Script de configuração do rootfs | executado contra uma cópia nova do rootfs; mesmos binários e runlevel do aparelho |
| Teste de estabilidade de 24 h | em andamento na data em que isto foi escrito |

## O que há aqui

| Caminho | O quê |
|---|---|
| [docs/TUTORIAL.pt-BR.md](docs/TUTORIAL.pt-BR.md) | O guia completo: backups, escolha da faixa de armazenamento, build, gravação, primeiro boot, recuperação, armadilhas |
| `src/initramfs/` | PID 1 do initramfs: monta em loop o rootfs ext4 da `super`, SSH de resgate por serial USB, watchdog de boot |
| `src/wifi/` | Ativação da conectividade do MT6877 (entrega a sua própria calibração ao driver do fabricante) |
| `src/thermal/` | Guarda térmico e daemon da janela de carga 60–80 %, com testes offline |
| `src/tools/` | Utilitário para reiniciar no Fastboot |
| `rootfs/` | Serviços OpenRC, modelo de firewall, config do sshd (valores de rede são `@PLACEHOLDERS@`) |
| `tools/` | Montador da imagem de boot, configurador do rootfs, verificador da faixa da super, proxy SSH serial, testes de reinício e estabilidade, auditoria de privacidade |
| `port/` | Pacotes pmbootstrap de dispositivo e kernel usados para compilar o kernel |

## Não incluído, de propósito

Firmware do fabricante, a sua calibração de Wi-Fi, imagens de disco e de boot,
credenciais de Wi-Fi, chaves e logs. O tutorial mostra como extrair o firmware e a
calibração do **seu próprio** aparelho. O `tools/audit-privacy.py` confere a árvore
contra vazamentos antes de cada commit.

## Caminho rápido

1. Faça backup de tudo e confira os hashes (tutorial, seção 3).
2. `python3 tools/check-super-range.py --active lp-ativo.txt --inactive lp-inativo.txt`
3. Compile kernel, utilitários e imagem de boot (seções 6-7).
4. Configure o rootfs, crie e grave a imagem (seções 8-9).
5. Grave o `boot_b`, confirme em até 600 s e ensaie a volta (seções 10-11).

## Créditos e licenças

Construído sobre [postmarketOS](https://postmarketos.org), Alpine Linux, OpenRC,
OpenSSH, nftables e `wpa_supplicant`, e sobre a árvore pública "hydrogen" do MT6877
citada em `port/linux-xiaomi-pissarro/APKBUILD`. Nosso código é MIT (veja
[LICENSE](LICENSE)); os arquivos derivados do kernel Linux em
`port/linux-xiaomi-pissarro/` são GPL-2.0-only.
