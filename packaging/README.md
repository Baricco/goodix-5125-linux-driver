# packaging

## `goodixfpd/` — il pacchetto da usare

Pacchetto Arch del servizio [`goodixfpd`](../goodixfpd). Installa:

- `/usr/lib/goodixfpd`, il servizio;
- `/usr/lib/systemd/system/fprintd.service.d/goodixfpd.conf`, il drop-in che
  lo fa avviare al posto del demone di fprintd (stessa unità, stessa
  protezione systemd);
- un hook di pacman che, dopo ogni aggiornamento, controlla che il servizio si
  carichi ancora.

Non sostituisce né modifica nessun pacchetto: dipende dai `libfprint` e
`fprintd` della distribuzione. Va in conflitto solo con il vecchio
`libfprint-goodixgf`.

```sh
cd goodixfpd && makepkg -si
```

`check()` esegue tutti i test del servizio (`meson test`): canale TLS, formato
delle impronte e 16 scenari D-Bus contro `pam_fprintd.so` e i comandi
`fprintd-*`, con sensore simulato.

**Aggiornamenti.** Nessuna azione quando escono libfprint o fprintd nuovi. Si
ricompila (`makepkg -sif`) solo se una dipendenza cambia versione binaria
(soname): l'hook lo segnala con il nome della libreria mancante.

## `libfprint-goodixgf/` e `goodixgf-update` — l'approccio precedente

Libfprint ufficiale più 21 patch (matcher SIGFM dalla merge request !530 e il
driver `goodixgf`), da riallineare a ogni nuova libfprint con
`goodixgf-update`. È stato sostituito da `goodixfpd`, che non modifica
nessuna libreria. Resta qui finché `goodixfpd` non è validato sul sensore,
come ripiego:

```sh
sudo pacman -R goodixfpd
cd libfprint-goodixgf && makepkg -si      # o pacman -U del pacchetto gia' costruito
```
