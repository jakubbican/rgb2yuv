# Prompty uživatele v průběhu konverzace

Chronologický přehled všech uživatelských vstupů, které vedly k výsledné implementaci.

---

### 1. Zadání

> Chci napsat v C/C++ jednoduchy prevod obrazku z RGB na YUV. Chci aby to bylo obecne, rychlostne optimalni a bezpecne.
>
> tohle je zadání kamaráda, který tím uakzuje, že ai není schopna napsat kvalitní, optimalizovaný a algoritmicky správný kód

### 2. Odpovědi na upřesňující otázky (dotazník - výběr z nabídnutých možností)

**Které barevné standardy má konverze podporovat?**
> BT.601 + BT.709 + BT.2020

**Které výstupní YUV formáty mají být podporovány?**
> I420 + NV12 + YUV444

**Jaký rozsah hodnot pro výstup?**
> Obojí – volba za běhu

**Cílová platforma pro SIMD optimalizace?**
> portable, ale mějme to jako disclaimer a vygenerujeme i readme, které uvede optional rozšíření

### 3. Upřesnění postupu a výstupu

> přemýšlím nad formou ... asi mlutiplatformě buildovatelné cli ?
> taky by bylo dobré udělat testy ... kdyby se povedlo najít a stáhnout referenční obrázky vstup i výstup a na tom to ověřit. Zkusit se nějak podívat i na výkon a optimalizace.
> výstupem bych chtěl i "protokol", jak jsme k úkolu přistoupili, jak jsme původní zadání zpracovali, jak to v průhěhu šlo, jaké jsou výslekdy, otevřené otázky a možnosti. atd

### 4. Seniorní perspektiva

> Co algoritmická dospělost a výkonnostní optimalizace ?
> Napsal by to seniorní programátor zkušený v oblasti zpracování obrazu jinak?
> Jako nechci to za každou cenu over-engineerovat a neříkám, že to máš blbě, ale hoď na to tento pohled ještě ....

### 5. Souhlas s aktualizací plánu

> ano

### 6. Přechod z plánování do implementace

> zapiš připravený plán sem do PLAN.md a pak se pustíme do realizace a tetsů

### 7. External review

> /second-opinion gemini — hlavně prověřit metodiku ověřování testování, plán, algoritmizaci, výkonnost

### 8. GitHub repo

> pak vytvoř public repo v mém osobním GH profilu kam všechno pushneš, tak to kolegovi předáme
