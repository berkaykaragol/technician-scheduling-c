#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TEKNISYEN_SAYISI 4
#define MAX_TUR_UZUNLUK  39        /* GOREV_TURU en fazla 39 karakter */
#define MAX_DEGER        1000000L  /* sayisal alanlar icin ust sinir */
#define SAYFA_BOYUTU     25
#define KONUM_SAYISI     5

/* Gecerli konumlar (hem dosya dogrulamasinda hem 4. algoritmada kullanilir) */
const char *KONUMLAR[KONUM_SAYISI] = {"LAB_A", "LAB_B", "DERSLIK_1", "DERSLIK_2", "DERSLIK_3"};

/* Konum degisim suresi: 1 = konum degisince 1 birim gecis suresi var, 0 = yok */
int gecis_suresi_var = 1;

typedef enum {
    HENUZ_GELMEDI = 0,
    BEKLIYOR      = 1,
    DEVAM_EDIYOR  = 2,
    TAMAMLANDI    = 3
} gorev_durumu;

typedef struct {
    /* Sabit degiskenler */
    int id;
    int gelis_zamani;
    int sure;
    int oncelik;
    char gorev_turu[40];
    char konum[20];

    /* Dinamik degiskenler */
    int kalan_sure;
    gorev_durumu durum;
    int guncel_teknisyen;   /* 0 = teknisyen yok */
    int baslangic_zamani;   /* -1 = henuz baslamadi */
    int bitis_zamani;       /* -1 = henuz bitmedi */
} gorev;

typedef struct {
    int id;
    int mesgulluk;          /* 1 = bir gorevi var (gecis suresi dahil) */
    int guncel_gorev_id;    /* 0 = gorev yok */
    int gorev_index;        /* aktif_liste icindeki sira, -1 = yok */
    char son_konum[20];     /* bos string = daha once gorev yapmadi */
    int gecis_durum;        /* 1 = su an konum degisimi (gecis) suresinde */
    int calisilan_birim;    /* istatistik: gercekten calisilan birim */
    int quantum_sayac;      /* Round Robin dilim sayaci */
} teknisyen;

/* ---------- Genel durum (global) ---------- */
gorev *ana_liste = NULL;      /* dosyadan okunan sabit veri */
gorev *aktif_liste = NULL;    /* simulasyonun degisen kopyasi */
int toplam_gorev_sayisi = 0;

teknisyen teknisyenler[TEKNISYEN_SAYISI];

/* Round Robin icin tek ortak FIFO kuyruk (dairesel dizi) */
int *kuyruk = NULL;
int kuyruk_bas = 0, kuyruk_son = 0, kuyruk_adet = 0;

int guncel_zaman = 0;
int secili_algoritma = 1;
int quantum = 0;
int log_acik = 1;             /* atama mesajlarini yazdir */

/* ---------- Yardimci fonksiyonlar ---------- */
void Cikis_Yap(int kod) {
    free(aktif_liste);
    free(ana_liste);
    free(kuyruk);
    exit(kod);
}

/* Dogrulanmis tam sayi okur; gecersizse tekrar sorar. */
long Sayi_Al(const char *soru_metni, long alt_sinir, long ust_sinir) {
    char satir[64];
    while (1) {
        printf("%s", soru_metni);
        if (fgets(satir, sizeof(satir), stdin) == NULL) {
            printf("\nGiris sonlandi, program kapatiliyor.\n");
            Cikis_Yap(0);
        }
        if (strchr(satir, '\n') == NULL) {   /* cok uzun giris */
            int karakter;
            while ((karakter = getchar()) != '\n' && karakter != EOF) { }
            printf("Gecersiz giris, tekrar deneyin.\n");
            continue;
        }
        char *sayinin_sonu;
        errno = 0;
        long deger = strtol(satir, &sayinin_sonu, 10);
        int rakam_var = (sayinin_sonu != satir);
        while (*sayinin_sonu == ' ' || *sayinin_sonu == '\t' || *sayinin_sonu == '\r') sayinin_sonu++;
        if (!rakam_var || *sayinin_sonu != '\n' || errno != 0 || deger < alt_sinir || deger > ust_sinir) {
            printf("Gecersiz giris (%ld ile %ld arasinda bir tam sayi giriniz).\n", alt_sinir, ust_sinir);
            continue;
        }
        return deger;
    }
}

const char *Durum_Adi(gorev_durumu durum) {
    switch (durum) {
        case HENUZ_GELMEDI: return "HENUZ GELMEDI";
        case BEKLIYOR:      return "BEKLIYOR";
        case DEVAM_EDIYOR:  return "DEVAM EDIYOR";
        case TAMAMLANDI:    return "TAMAMLANDI";
    }
    return "?";
}

const char *Algoritma_Adi(int algoritma_no) {
    switch (algoritma_no) {
        case 1: return "Shortest Job";
        case 2: return "Priority";
        case 3: return "Round Robin";
        case 4: return "Konuma Duyarli (Konum Odakli SJ)";
    }
    return "?";
}

void Durum_Sayilari(int durum_adetleri[4]) {
    durum_adetleri[0] = durum_adetleri[1] = durum_adetleri[2] = durum_adetleri[3] = 0;
    for (int i = 0; i < toplam_gorev_sayisi; i++) durum_adetleri[aktif_liste[i].durum]++;
}

/* "Toplam | Henuz gelmedi | Bekliyor | Devam ediyor | Tamamlandi" satirini yazar */
void Durum_Ozeti_Yazdir(int durum_adetleri[4]) {
    printf("Toplam: %d | Henuz gelmedi: %d | Bekliyor: %d | Devam ediyor: %d | Tamamlandi: %d\n",
           toplam_gorev_sayisi, durum_adetleri[0], durum_adetleri[1], durum_adetleri[2], durum_adetleri[3]);
}

/* 0 dondurursa kullanici listeyi kesmek istemistir */
int Sayfa_Sonu_Sor(void) {
    char cevap[16];
    printf("-- Devam icin Enter, cikmak icin q: ");
    if (fgets(cevap, sizeof(cevap), stdin) == NULL) return 0;
    return !(cevap[0] == 'q' || cevap[0] == 'Q');
}

/* ---------- Dosya okuma ---------- */
int Konum_Gecerli_Mi(const char *konum) {
    for (int i = 0; i < KONUM_SAYISI; i++)
        if (strcmp(konum, KONUMLAR[i]) == 0) return 1;
    return 0;
}

int Tam_Sayi_Cevir(const char *yazi, long *cevrilen_sayi) {
    char *sayinin_sonu;
    errno = 0;
    *cevrilen_sayi = strtol(yazi, &sayinin_sonu, 10);
    return (*yazi != '\0' && *sayinin_sonu == '\0' && errno == 0);
}

void Dosya_Oku(void) {
    char dosya_yolu[256];
    printf("Lutfen dosya yolunu giriniz ('C:\\...\\gorevler.txt' seklinde): ");
    if (fgets(dosya_yolu, sizeof(dosya_yolu), stdin) == NULL) exit(1);
    dosya_yolu[strcspn(dosya_yolu, "\r\n")] = '\0';

    /* Yol tirnak icindeyse ("..." veya '...') bastaki ve sondaki tirnagi at */
    int path_uzunluk = strlen(dosya_yolu);
    char *yol = dosya_yolu;
    if (path_uzunluk >= 2 &&
        ((dosya_yolu[0] == '"'  && dosya_yolu[path_uzunluk - 1] == '"') ||
         (dosya_yolu[0] == '\'' && dosya_yolu[path_uzunluk - 1] == '\''))) {
        yol++;                                   /* bastaki tirnagi atla */
        dosya_yolu[path_uzunluk - 1] = '\0';     /* sondaki tirnagi sil */
    }

    FILE *dosya = fopen(yol, "r");
    if (dosya == NULL) {
        printf("'%s' bulunamadi veya acilamadi.\n", yol);
        exit(1);
    }

    /* Dosya UTF-8 BOM (EF BB BF) ile basliyorsa 3 byte'i atla; baslamiyorsa basa don */
    unsigned char ilk_baytlar[3];
    size_t okunan_bayt = fread(ilk_baytlar, 1, 3, dosya);
    int bom_var = (okunan_bayt == 3 && ilk_baytlar[0] == 0xEF && ilk_baytlar[1] == 0xBB && ilk_baytlar[2] == 0xBF);
    if (!bom_var) rewind(dosya);   /* okunan 3 byte gercek veriydi, okuma konumunu dosya basina al */

    int kapasite = 1000;
    ana_liste = (gorev *) malloc(kapasite * sizeof(gorev));
    if (ana_liste == NULL) {
        printf("Bellek ayirmada problem yasandi.\n");
        fclose(dosya);
        exit(1);
    }

    char satir[512];
    int satir_no = 0;
    toplam_gorev_sayisi = 0;

    while (fgets(satir, sizeof(satir), dosya)) {
        satir_no++;

        /* Tampon tasmasi: satir tamamen okunamadiysa kalani atilir */
        if (strchr(satir, '\n') == NULL && !feof(dosya)) {
            int karakter;
            while ((karakter = fgetc(dosya)) != '\n' && karakter != EOF) { }
            printf("Satir %d reddedildi: satir cok uzun.\n", satir_no);
            continue;
        }

        /* Bos satir ve aciklama satirlarini atla */
        char *satir_basi = satir;
        while (*satir_basi == ' ' || *satir_basi == '\t') satir_basi++;
        if (*satir_basi == '\0' || *satir_basi == '\n' || *satir_basi == '\r' || *satir_basi == '#') continue;

        /* Alanlara ayir */
        char *alanlar[7];
        int alan_sayisi = 0;
        char *kelime = strtok(satir_basi, " \t\r\n");
        while (kelime != NULL) {
            if (alan_sayisi < 7) alanlar[alan_sayisi] = kelime;
            alan_sayisi++;
            kelime = strtok(NULL, " \t\r\n");
        }
        if (alan_sayisi != 6) {
            printf("Satir %d reddedildi: alan sayisi 6 olmali (bulunan: %d).\n", satir_no, alan_sayisi);
            continue;
        }

        long gecici_id, gecici_gelis, gecici_sure, gecici_oncelik;
        const char *hata_mesaji = NULL;
        if (!Tam_Sayi_Cevir(alanlar[0], &gecici_id) || gecici_id <= 0 || gecici_id > MAX_DEGER)
            hata_mesaji = "ID pozitif bir tam sayi olmali";
        else if (!Tam_Sayi_Cevir(alanlar[1], &gecici_gelis) || gecici_gelis < 0 || gecici_gelis > MAX_DEGER)
            hata_mesaji = "GELIS_ZAMANI sifir veya daha buyuk bir tam sayi olmali";
        else if (!Tam_Sayi_Cevir(alanlar[2], &gecici_sure) || gecici_sure <= 0 || gecici_sure > MAX_DEGER)
            hata_mesaji = "SURE sifirdan buyuk bir tam sayi olmali";
        else if (!Tam_Sayi_Cevir(alanlar[3], &gecici_oncelik) || gecici_oncelik < 1 || gecici_oncelik > 5)
            hata_mesaji = "ONCELIK 1 ile 5 arasinda olmali";
        else if (strlen(alanlar[4]) > MAX_TUR_UZUNLUK)
            hata_mesaji = "GOREV_TURU en fazla 39 karakter olmali";
        else if (!Konum_Gecerli_Mi(alanlar[5]))
            hata_mesaji = "KONUM LAB_A, LAB_B, DERSLIK_1, DERSLIK_2 veya DERSLIK_3 olmali";

        if (hata_mesaji != NULL) {
            printf("Satir %d reddedildi: %s.\n", satir_no, hata_mesaji);
            continue;
        }

        int id_cakismasi = 0;
        for (int onceki_gorev = 0; onceki_gorev < toplam_gorev_sayisi; onceki_gorev++) {
            if (ana_liste[onceki_gorev].id == (int) gecici_id) { id_cakismasi = 1; break; }
        }
        if (id_cakismasi) {
            printf("Satir %d reddedildi: ID (%ld) daha once kaydedilmis (ilk kayit korunur).\n", satir_no, gecici_id);
            continue;
        }

        if (toplam_gorev_sayisi >= kapasite) {
            kapasite *= 2;
            gorev *yeni_alan = (gorev *) realloc(ana_liste, kapasite * sizeof(gorev));
            if (yeni_alan == NULL) {
                printf("Bellek dolu, alan arttirilamiyor! Okuma burada durduruldu.\n");
                break;
            }
            ana_liste = yeni_alan;
        }

        gorev *yeni_gorev = &ana_liste[toplam_gorev_sayisi];
        yeni_gorev->id = (int) gecici_id;
        yeni_gorev->gelis_zamani = (int) gecici_gelis;
        yeni_gorev->sure = (int) gecici_sure;
        yeni_gorev->kalan_sure = (int) gecici_sure;
        yeni_gorev->oncelik = (int) gecici_oncelik;
        strcpy(yeni_gorev->gorev_turu, alanlar[4]);
        strcpy(yeni_gorev->konum, alanlar[5]);
        yeni_gorev->baslangic_zamani = -1;
        yeni_gorev->bitis_zamani = -1;
        yeni_gorev->guncel_teknisyen = 0;
        yeni_gorev->durum = HENUZ_GELMEDI;
        toplam_gorev_sayisi++;
    }
    fclose(dosya);
    printf("Dosya okundu. Toplam %d gecerli gorev yuklendi.\n", toplam_gorev_sayisi);
}

int Id_Karsilastir(const void *birinci, const void *ikinci) {
    const gorev *gorev_1 = (const gorev *) birinci;
    const gorev *gorev_2 = (const gorev *) ikinci;
    return (gorev_1->id > gorev_2->id) - (gorev_1->id < gorev_2->id);
}

/* ---------- Round Robin kuyrugu ---------- */
void Kuyruga_Ekle(int gorev_index) {
    kuyruk[kuyruk_son] = gorev_index;
    kuyruk_son = (kuyruk_son + 1) % toplam_gorev_sayisi;
    kuyruk_adet++;
}

int Kuyruktan_Al(void) {
    if (kuyruk_adet == 0) return -1;
    int gorev_index = kuyruk[kuyruk_bas];
    kuyruk_bas = (kuyruk_bas + 1) % toplam_gorev_sayisi;
    kuyruk_adet--;
    return gorev_index;
}

/* Zamani gelen gorevleri Bekliyor yapar; aktif_liste ID'ye gore sirali oldugu
   icin ayni anda gelenler kucuk ID once sirasiyla kuyruga girer. */
void Gelisleri_Isle(void) {
    for (int i = 0; i < toplam_gorev_sayisi; i++) {
        if (aktif_liste[i].durum == HENUZ_GELMEDI && aktif_liste[i].gelis_zamani <= guncel_zaman) {
            aktif_liste[i].durum = BEKLIYOR;
            if (secili_algoritma == 3) Kuyruga_Ekle(i);
        }
    }
}

/* ---------- Gorev secimi ---------- */
/* gorev_a, gorev_b'den once secilmeliyse 1 doner */
int Once_SJ(const gorev *gorev_a, const gorev *gorev_b) {
    if (gorev_a->sure != gorev_b->sure) return gorev_a->sure < gorev_b->sure;
    if (gorev_a->gelis_zamani != gorev_b->gelis_zamani) return gorev_a->gelis_zamani < gorev_b->gelis_zamani;
    return gorev_a->id < gorev_b->id;
}

int Once_Priority(const gorev *gorev_a, const gorev *gorev_b) {
    if (gorev_a->oncelik != gorev_b->oncelik) return gorev_a->oncelik < gorev_b->oncelik;
    if (gorev_a->gelis_zamani != gorev_b->gelis_zamani) return gorev_a->gelis_zamani < gorev_b->gelis_zamani;
    return gorev_a->id < gorev_b->id;
}

int Once_FCFS(const gorev *gorev_a, const gorev *gorev_b) {
    if (gorev_a->gelis_zamani != gorev_b->gelis_zamani) return gorev_a->gelis_zamani < gorev_b->gelis_zamani;
    return gorev_a->id < gorev_b->id;
}

/* Bekleyen gorevler arasindan "kural" fonksiyonuna gore en iyi olanin sira numarasini (index) verir.
   konum_filtresi NULL degilse sadece o konumdaki gorevlere bakar (NULL = her konum olur). */
int Sec_Bekleyen(int (*kural)(const gorev *, const gorev *), const char *konum_filtresi) {
    int secilen = -1;
    for (int i = 0; i < toplam_gorev_sayisi; i++) {
        if (aktif_liste[i].durum != BEKLIYOR) continue;
        if (aktif_liste[i].guncel_teknisyen != 0) continue;   /* gecis suresinde bir teknisyene ayrilmis */
        if (konum_filtresi != NULL && strcmp(aktif_liste[i].konum, konum_filtresi) != 0) continue;

        if (secilen == -1 || kural(&aktif_liste[i], &aktif_liste[secilen]))
            secilen = i;
    }
    return secilen;
}

/* 4. algoritma: Konuma Duyarli (Konum Odakli SJ)
   Tum teknisyenler ayni hizda calistigi icin isin bitis suresini sadece konum degistirme
   (1 birim gecis) etkiler. Amac: gereksiz konum degisimini azaltmak.
     1) Teknisyenin bulundugu konumda bekleyen is varsa onu al (gecis yok), en kisa sureli olani sec.
     2) Yoksa bekleyen isi (kalan sure toplami olarak) en cok olan konuma git.
        Boylece teknisyen yeni konumda uzun sure kalir, cok sik yer degistirmez.
     3) O konumda yine en kisa sureli isi sec (SJ). */
int Sec_Konuma_Duyarli(const teknisyen *bu_teknisyen) {
    /* 1) Bulundugu konumda is var mi? */
    if (bu_teknisyen->son_konum[0] != '\0') {
        int ayni_konumdaki = Sec_Bekleyen(Once_SJ, bu_teknisyen->son_konum);
        if (ayni_konumdaki != -1) return ayni_konumdaki;
    }

    /* 2) Her konumdaki bekleyen islerin toplam suresini hesapla */
    long konum_yukleri[KONUM_SAYISI] = {0};
    for (int i = 0; i < toplam_gorev_sayisi; i++) {
        if (aktif_liste[i].durum != BEKLIYOR) continue;
        if (aktif_liste[i].guncel_teknisyen != 0) continue;   /* gecis suresinde bir teknisyene ayrilmis */
        for (int k = 0; k < KONUM_SAYISI; k++) {
            if (strcmp(aktif_liste[i].konum, KONUMLAR[k]) == 0) konum_yukleri[k] += aktif_liste[i].kalan_sure;
        }
    }

    int en_yuklu_konum = -1;
    for (int k = 0; k < KONUM_SAYISI; k++) {
        if (konum_yukleri[k] == 0) continue;     /* orada bekleyen is yok */
        if (en_yuklu_konum == -1 || konum_yukleri[k] > konum_yukleri[en_yuklu_konum]) en_yuklu_konum = k;
    }
    if (en_yuklu_konum == -1) return -1;         /* hic bekleyen is yok */

    /* 3) O konumda en kisa isi sec */
    return Sec_Bekleyen(Once_SJ, KONUMLAR[en_yuklu_konum]);
}

int Gorev_Sec(const teknisyen *bu_teknisyen) {
    switch (secili_algoritma) {
        case 1:  return Sec_Bekleyen(Once_SJ, NULL);
        case 2:  return Sec_Bekleyen(Once_Priority, NULL);
        case 3:  return Kuyruktan_Al();
        default: return Sec_Konuma_Duyarli(bu_teknisyen);   /* 4 */
    }
}

/* Bos teknisyenlere T1..T4 sirasiyla gorev atar.
   Gorevin konumu teknisyenin son konumundan farkliysa teknisyen 1 birim gecis suresine girer. */
void Atama_Yap(void) {
    for (int teknisyen_no = 0; teknisyen_no < TEKNISYEN_SAYISI; teknisyen_no++) {
        teknisyen *bu_teknisyen = &teknisyenler[teknisyen_no];
        if (bu_teknisyen->mesgulluk) continue;       /* mesgul teknisyeni atla */

        int gorev_index = Gorev_Sec(bu_teknisyen);
        if (gorev_index == -1) break;                /* secilecek gorev kalmadi */
        gorev *bu_gorev = &aktif_liste[gorev_index];

        /* 1) Gorevi teknisyene bagla (durumu asagida, gecis olup olmadigina gore belirlenecek) */
        bu_gorev->guncel_teknisyen = bu_teknisyen->id;

        /* 2) Teknisyeni guncelle */
        bu_teknisyen->mesgulluk = 1;
        bu_teknisyen->guncel_gorev_id = bu_gorev->id;
        bu_teknisyen->gorev_index = gorev_index;
        bu_teknisyen->quantum_sayac = 0;

        /* 3) Konum degistiyse 1 birimlik gecis suresi baslat */
        if (gecis_suresi_var && bu_teknisyen->son_konum[0] != '\0' && strcmp(bu_teknisyen->son_konum, bu_gorev->konum) != 0)
            bu_teknisyen->gecis_durum = 1;
        else
            bu_teknisyen->gecis_durum = 0;
        strcpy(bu_teknisyen->son_konum, bu_gorev->konum);

        /* Karar: gecis suresindeki teknisyen MESGUL, gorevi ise BEKLIYOR kalir (secilemez, cunku
           guncel_teknisyen dolu). Gecis yoksa gorev hemen DEVAM EDIYOR olur. */
        if (bu_teknisyen->gecis_durum == 0) {
            bu_gorev->durum = DEVAM_EDIYOR;
            if (bu_gorev->baslangic_zamani == -1) bu_gorev->baslangic_zamani = guncel_zaman;
        } else {
            bu_gorev->durum = BEKLIYOR;
        }

        /* 4) Ekrana yaz */
        if (log_acik) {
            printf(" -> Sistem: Zaman %d'de [T%d] teknisyenine %d ID'li gorev atandi%s.\n",
                   guncel_zaman, bu_teknisyen->id, bu_gorev->id, bu_teknisyen->gecis_durum ? " (konum degisimi: 1 birim gecis)" : "");
        }
    }
}

/* ---------- Simulasyon ---------- */
/* Teknisyenin uzerindeki gorevi ve sayaclarini sifirlar; teknisyen bosta olur.
   Baslat'tan ONCE yazildi, cunku C'de bir fonksiyon cagrilmadan once tanimlanmis olmali. */
void Teknisyeni_Bosalt(int teknisyen_no) {
    teknisyenler[teknisyen_no].mesgulluk = 0;
    teknisyenler[teknisyen_no].guncel_gorev_id = 0;
    teknisyenler[teknisyen_no].gorev_index = -1;
    teknisyenler[teknisyen_no].quantum_sayac = 0;
    teknisyenler[teknisyen_no].gecis_durum = 0;
}

void Baslat(void) {
    memcpy(aktif_liste, ana_liste, toplam_gorev_sayisi * sizeof(gorev));
    for (int teknisyen_no = 0; teknisyen_no < TEKNISYEN_SAYISI; teknisyen_no++) {
        teknisyenler[teknisyen_no].id = teknisyen_no + 1;
        teknisyenler[teknisyen_no].son_konum[0] = '\0';
        teknisyenler[teknisyen_no].calisilan_birim = 0;
        Teknisyeni_Bosalt(teknisyen_no);   /* gorev ve sayaclari sifirla */
    }
    kuyruk_bas = kuyruk_son = kuyruk_adet = 0;
    guncel_zaman = 0;

    Gelisleri_Isle();   /* zaman 0 gelisleri */
    Atama_Yap();        /* zaman 0 ilk atamalari */
}

/* Bir birim zaman ilerletir. Sira (PDF'e gore):
   calisma -> saat -> biten / quantum dolan -> yeni gelisler -> dolanlarin kuyruga donusu -> atama */
void Adim_Ilerlet(void) {
    int suresi_dolanlar[TEKNISYEN_SAYISI];
    int suresi_dolan_sayisi = 0;

    /* 1) Calisma: gecis suresindeki teknisyende is ilerlemez, quantum islemez */
    for (int teknisyen_no = 0; teknisyen_no < TEKNISYEN_SAYISI; teknisyen_no++) {
        teknisyen *bu_teknisyen = &teknisyenler[teknisyen_no];
        if (!bu_teknisyen->mesgulluk) continue;
        if (bu_teknisyen->gecis_durum) {
            /* Gecis bitti: gorev bir sonraki zaman diliminde fiilen calismaya baslar */
            bu_teknisyen->gecis_durum = 0;
            gorev *gecisi_biten_gorev = &aktif_liste[bu_teknisyen->gorev_index];
            gecisi_biten_gorev->durum = DEVAM_EDIYOR;
            if (gecisi_biten_gorev->baslangic_zamani == -1) gecisi_biten_gorev->baslangic_zamani = guncel_zaman + 1;
        } else {
            aktif_liste[bu_teknisyen->gorev_index].kalan_sure--;
            bu_teknisyen->calisilan_birim++;
            bu_teknisyen->quantum_sayac++;
        }
    }

    /* 2) Saat */
    guncel_zaman++;

    /* 3) Biten gorevler ve (RR) quantum'u dolanlar; T1..T4 sirasiyla */
    for (int teknisyen_no = 0; teknisyen_no < TEKNISYEN_SAYISI; teknisyen_no++) {
        teknisyen *bu_teknisyen = &teknisyenler[teknisyen_no];
        if (!bu_teknisyen->mesgulluk) continue;
        gorev *bu_gorev = &aktif_liste[bu_teknisyen->gorev_index];

        if (bu_gorev->kalan_sure == 0) {
            bu_gorev->durum = TAMAMLANDI;
            bu_gorev->bitis_zamani = guncel_zaman;   /* guncel_teknisyen son bitiren olarak kalir */
            Teknisyeni_Bosalt(teknisyen_no);
        } else if (secili_algoritma == 3 && bu_teknisyen->quantum_sayac >= quantum) {
            bu_gorev->durum = BEKLIYOR;
            bu_gorev->guncel_teknisyen = 0;
            suresi_dolanlar[suresi_dolan_sayisi++] = bu_teknisyen->gorev_index;
            if (log_acik)
                printf(" -> Sistem: Zaman %d'de %d ID'li gorev quantum doldurdu, kuyruga dondu.\n",
                       guncel_zaman, bu_gorev->id);
            Teknisyeni_Bosalt(teknisyen_no);
        }
    }

    /* 4) Yeni gelisler (kuyrugun sonuna, kucuk ID once) */
    Gelisleri_Isle();

    /* 5) Quantum dolan gorevler, teknisyen numarasi sirasiyla kuyrugun sonuna */
    for (int i = 0; i < suresi_dolan_sayisi; i++) Kuyruga_Ekle(suresi_dolanlar[i]);

    /* 6) Bos teknisyenlere atama */
    Atama_Yap();
}

/* hedef zamana gider; geriye gidilecekse basindan yeniden hesaplar */
void Zamana_Git(int hedef_zaman) {
    if (hedef_zaman < guncel_zaman) {
        log_acik = 0;      /* tekrar hesaplama sirasinda mesaj basma */
        Baslat();
    }
    while (guncel_zaman < hedef_zaman) Adim_Ilerlet();
    log_acik = 1;
}

/* ---------- Ekranlar ---------- */
void Gorev_Listeleri(void) {
    int durum_adetleri[4];
    Durum_Sayilari(durum_adetleri);

    printf("\n==================================================\n");
    printf("                  GOREV LISTESI                   \n");
    printf("==================================================\n");
    for (int i = 0; i < toplam_gorev_sayisi; i++) {
        gorev *bu_gorev = &aktif_liste[i];
        char teknisyen_yazisi[16] = "-";
        if (bu_gorev->guncel_teknisyen > 0 && bu_gorev->durum != BEKLIYOR) snprintf(teknisyen_yazisi, sizeof(teknisyen_yazisi), "T%d", bu_gorev->guncel_teknisyen);

        printf("ID: %-5d | Gelis: %-6d | Sure: %-4d | Oncelik: %-2d | Durum: %-13s | Teknisyen: %-3s | Kalan: %-4d\n",
               bu_gorev->id, bu_gorev->gelis_zamani, bu_gorev->sure, bu_gorev->oncelik, Durum_Adi(bu_gorev->durum), teknisyen_yazisi, bu_gorev->kalan_sure);

        if ((i + 1) % SAYFA_BOYUTU == 0 && i + 1 < toplam_gorev_sayisi) {
            if (!Sayfa_Sonu_Sor()) break;
        }
    }
    printf("--------------------------------------------------\n");
    Durum_Ozeti_Yazdir(durum_adetleri);
}

void Gorev_Durum_Liste(void) {
    int durum_adetleri[4];
    Durum_Sayilari(durum_adetleri);
    int yazdirilan = 0;
    int listeyi_kes = 0;

    for (int durum_no = 0; durum_no < 4 && !listeyi_kes; durum_no++) {
        switch (durum_no) {
            case HENUZ_GELMEDI: printf("\n --- Henuz Gelmeyen Gorevler ---\n"); break;
            case BEKLIYOR:      printf("\n --- Beklemede Olan Gorevler ---\n"); break;
            case DEVAM_EDIYOR:  printf("\n --- Devam Eden Gorevler ---\n"); break;
            case TAMAMLANDI:    printf("\n --- Tamamlanan Gorevler ---\n"); break;
        }
        if (durum_adetleri[durum_no] == 0) {
            printf("Bu kategoride gorev bulunmamaktadir.\n");
            continue;
        }
        for (int i = 0; i < toplam_gorev_sayisi; i++) {
            gorev *bu_gorev = &aktif_liste[i];
            if ((int) bu_gorev->durum != durum_no) continue;

            printf("ID: %-5d | Gelis: %-6d | Sure: %-4d | Oncelik: %-2d | Kalan: %-4d",
                   bu_gorev->id, bu_gorev->gelis_zamani, bu_gorev->sure, bu_gorev->oncelik, bu_gorev->kalan_sure);
            if (bu_gorev->guncel_teknisyen > 0 && bu_gorev->durum != BEKLIYOR) printf(" | Teknisyen: T%d", bu_gorev->guncel_teknisyen);
            printf("\n");

            yazdirilan++;
            if (yazdirilan % SAYFA_BOYUTU == 0 && yazdirilan < toplam_gorev_sayisi) {
                if (!Sayfa_Sonu_Sor()) { listeyi_kes = 1; break; }
            }
        }
    }
    printf("\n==================================================\n");
    Durum_Ozeti_Yazdir(durum_adetleri);
}

void Gorev_ID_Sorgulama(void) {
    int aranan = (int) Sayi_Al("Aramak istediginiz ID'yi giriniz: ", 1, MAX_DEGER);

    /* aktif_liste ID'ye gore sirali oldugundan ikili arama kullanilir */
    int sol = 0, sag = toplam_gorev_sayisi - 1;
    while (sol <= sag) {
        int orta = sol + (sag - sol) / 2;
        gorev *bulunan_gorev = &aktif_liste[orta];

        if (bulunan_gorev->id == aranan) {
            printf("\n--- Gorev Bulundu ---\n");
            printf("ID             : %d\n", bulunan_gorev->id);
            printf("Tur            : %s\n", bulunan_gorev->gorev_turu);
            printf("Konum          : %s\n", bulunan_gorev->konum);
            printf("Oncelik        : %d\n", bulunan_gorev->oncelik);
            printf("Gelis Zamani   : %d\n", bulunan_gorev->gelis_zamani);
            printf("Toplam Sure    : %d\n", bulunan_gorev->sure);
            printf("Kalan Sure     : %d\n", bulunan_gorev->kalan_sure);
            if (bulunan_gorev->baslangic_zamani == -1) printf("Ilk Baslama    : Baslamadi\n");
            else printf("Ilk Baslama    : %d\n", bulunan_gorev->baslangic_zamani);
            if (bulunan_gorev->bitis_zamani == -1) printf("Bitis Zamani   : Bitmedi\n");
            else printf("Bitis Zamani   : %d\n", bulunan_gorev->bitis_zamani);
            if (bulunan_gorev->guncel_teknisyen == 0 || bulunan_gorev->durum == BEKLIYOR) printf("Teknisyen      : -\n");
            else if (bulunan_gorev->durum == TAMAMLANDI) printf("Teknisyen      : T%d (son bitiren)\n", bulunan_gorev->guncel_teknisyen);
            else printf("Teknisyen      : T%d\n", bulunan_gorev->guncel_teknisyen);
            printf("Durum          : %s\n", Durum_Adi(bulunan_gorev->durum));
            return;
        }
        if (bulunan_gorev->id < aranan) sol = orta + 1;
        else sag = orta - 1;
    }
    printf("Aradiginiz ID'ye sahip gorev bulunamamistir.\n");
}

void Teknisyen_Ekrani(void) {
    printf("\n==================================================\n");
    printf("             TEKNISYEN DURUM EKRANI               \n");
    printf("==================================================\n");
    for (int teknisyen_no = 0; teknisyen_no < TEKNISYEN_SAYISI; teknisyen_no++) {
        teknisyen *bu_teknisyen = &teknisyenler[teknisyen_no];
        const char *durum_yazisi = !bu_teknisyen->mesgulluk ? "Bosta" : (bu_teknisyen->gecis_durum ? "Geciste" : "Mesgul");
        char gorev_id_yazisi[16] = "-", kalan_yazisi[16] = "-", dilimde_kalan_yazisi[16] = "-";

        if (bu_teknisyen->mesgulluk) {
            snprintf(gorev_id_yazisi, sizeof(gorev_id_yazisi), "%d", bu_teknisyen->guncel_gorev_id);
            snprintf(kalan_yazisi, sizeof(kalan_yazisi), "%d", aktif_liste[bu_teknisyen->gorev_index].kalan_sure);
            if (secili_algoritma == 3)
                snprintf(dilimde_kalan_yazisi, sizeof(dilimde_kalan_yazisi), "%d", quantum - bu_teknisyen->quantum_sayac);
        }
        printf("[T%d] Durum: %-7s | Gorev ID: %-6s | Kalan: %-5s | Dilimde kalan: %-5s | Calisilan: %-5d | Son konum: %s\n",
               bu_teknisyen->id, durum_yazisi, gorev_id_yazisi, kalan_yazisi, dilimde_kalan_yazisi, bu_teknisyen->calisilan_birim,
               bu_teknisyen->son_konum[0] != '\0' ? bu_teknisyen->son_konum : "-");
    }
    printf("==================================================\n");
}

void Istatistikleri_Goster(void) {
    int durum_adetleri[4];
    Durum_Sayilari(durum_adetleri);

    int tamamlanan_sayisi = 0;
    long toplam_bekleme = 0, toplam_sistemde = 0;
    int makespan = 0;

    for (int i = 0; i < toplam_gorev_sayisi; i++) {
        gorev *bu_gorev = &aktif_liste[i];
        if (bu_gorev->durum != TAMAMLANDI) continue;
        tamamlanan_sayisi++;
        toplam_bekleme += bu_gorev->bitis_zamani - bu_gorev->gelis_zamani - bu_gorev->sure;
        toplam_sistemde += bu_gorev->bitis_zamani - bu_gorev->gelis_zamani;
        if (bu_gorev->bitis_zamani > makespan) makespan = bu_gorev->bitis_zamani;
    }

    printf("\n==================================================\n");
    printf("               SISTEM ISTATISTIKLERI              \n");
    printf("==================================================\n");
    Durum_Ozeti_Yazdir(durum_adetleri);
    printf("\n");

    if (tamamlanan_sayisi == 0) {
        printf("Henuz tamamlanmis gorev yok, ortalamalar hesaplanamaz.\n");
    } else {
        printf("Tamamlanan Gorev Sayisi    : %d\n", tamamlanan_sayisi);
        printf("Makespan (Son Bitis Zamani): %d\n", makespan);
        printf("Ortalama Bekleme Suresi    : %.3f\n", (double) toplam_bekleme / tamamlanan_sayisi);
        printf("Ortalama Sistemde Kalma    : %.3f\n", (double) toplam_sistemde / tamamlanan_sayisi);
    }

    printf("\n--- TEKNISYEN KULLANIM ORANLARI ---\n");
    if (guncel_zaman == 0) {
        printf("Zaman 0 oldugu icin kullanim oranlari hesaplanamaz.\n");
    } else {
        long toplam_calisma = 0;
        for (int teknisyen_no = 0; teknisyen_no < TEKNISYEN_SAYISI; teknisyen_no++) {
            toplam_calisma += teknisyenler[teknisyen_no].calisilan_birim;
            printf("[T%d] Kullanim Orani: %%%.3f\n", teknisyenler[teknisyen_no].id,
                   (double) teknisyenler[teknisyen_no].calisilan_birim / guncel_zaman * 100.0);
        }
        printf("\nGENEL KULLANIM ORANI: %%%.3f\n",
               (double) toplam_calisma / ((double) guncel_zaman * TEKNISYEN_SAYISI) * 100.0);
    }
    printf("==================================================\n");
}

/* ---------- Menu ---------- */
void Algoritma_Sec(void) {
    printf("\n    *  1 - Shortest Job\n");
    printf("    *  2 - Priority\n");
    printf("    *  3 - Round Robin\n");
    printf("    *  4 - Konuma Duyarli (kendi algoritmam)\n");
    secili_algoritma = (int) Sayi_Al("    Lutfen istenilen algoritmanin numarasini giriniz: ", 1, 4);

    if (secili_algoritma == 3)
        quantum = (int) Sayi_Al("    Lutfen Round Robin icin quantum miktarini giriniz: ", 1, MAX_DEGER);
    else
        quantum = 0;

    Baslat();   /* simulasyon sifirlanir, zaman 0 atamalari yapilir */
}

int main(void) {
    Dosya_Oku();
    if (toplam_gorev_sayisi == 0) {
        printf("Gecerli gorev bulunamadigindan program kapatiliyor...\n");
        Cikis_Yap(1);
    }

    qsort(ana_liste, toplam_gorev_sayisi, sizeof(gorev), Id_Karsilastir);

    aktif_liste = (gorev *) malloc(toplam_gorev_sayisi * sizeof(gorev));
    kuyruk = (int *) malloc(toplam_gorev_sayisi * sizeof(int));
    if (aktif_liste == NULL || kuyruk == NULL) {
        printf("Bellek ayirmada problem yasandi.\n");
        Cikis_Yap(1);
    }

    Algoritma_Sec();

    while (1) {
        printf("\n   Secilen algoritma : %s", Algoritma_Adi(secili_algoritma));
        if (secili_algoritma == 3) printf(" (Quantum: %d)", quantum);
        printf("\n    Gecen zaman: %d | Konum degisim suresi: %s\n", guncel_zaman, gecis_suresi_var ? "VAR (1 birim)" : "YOK");
        printf(" 1-  o  Algoritma sec veya degistir\n");
        printf(" 2-  o  Tum gorevleri listele\n");
        printf(" 3-  o  Gorevleri durumuna gore listele\n");
        printf(" 4-  o  Gorev ID ile sorgula\n");
        printf(" 5-  o  Teknisyenlerin durumunu goster\n");
        printf(" 6-  o  Zamani 1 birim ilerlet\n");
        printf(" 7-  o  Zamani N birim ilerlet\n");
        printf(" 8-  o  Belirli bir zamana git\n");
        printf(" 9-  o  Istatistikleri goster\n");
        printf(" 10- o  Simulasyonu sifirla\n");
        printf(" 11- o  Cikis\n");

        int secim = (int) Sayi_Al("\n Yapmak istediginiz islemin numarasini giriniz: ", 1, 11);

        switch (secim) {
            case 1:
                Algoritma_Sec();
                break;
            case 2:
                Gorev_Listeleri();
                break;
            case 3:
                Gorev_Durum_Liste();
                break;
            case 4:
                Gorev_ID_Sorgulama();
                break;
            case 5:
                Teknisyen_Ekrani();
                break;
            case 6:
                printf(" Zaman 1 birim ilerletiliyor...\n");
                Zamana_Git(guncel_zaman + 1);
                break;
            case 7: {
                int ilerletilecek_birim = (int) Sayi_Al(" Kac birim ilerletmek istiyorsunuz (N > 0): ", 1, MAX_DEGER);
                printf(" Zaman %d birim ilerletiliyor...\n", ilerletilecek_birim);
                Zamana_Git(guncel_zaman + ilerletilecek_birim);
                break;
            }
            case 8: {
                int hedef_zaman = (int) Sayi_Al(" Gitmek istediginiz zamani giriniz (>= 0): ", 0, MAX_DEGER);
                if (hedef_zaman == guncel_zaman) {
                    printf(" Zaten bu zamandayiz, durum degismedi.\n");
                } else {
                    printf(" %d zamanina gidiliyor...\n", hedef_zaman);
                    Zamana_Git(hedef_zaman);
                }
                break;
            }
            case 9:
                Istatistikleri_Goster();
                break;
            case 10:
                Baslat();
                printf(" Simulasyon sifirlandi (zaman 0 atamalari yapildi).\n");
                break;
            case 11:
                Cikis_Yap(0);
                break;
        }
    }
    return 0;
}
