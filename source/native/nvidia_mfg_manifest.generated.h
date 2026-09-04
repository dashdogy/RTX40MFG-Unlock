#pragma once

#include <array>
#include <string_view>

struct ManifestEntry
{
    std::string_view key;
    Tier tier;
};

inline constexpr char kManifestFetchedDate[] = "2026-09-03";
inline constexpr char kManifestSha256[] = "8BBBA2D23CD2C76BCC50D6A9FDEB1CC8D6DEA289A6E423E04C882C9EB653C76F";
inline constexpr std::array<ManifestEntry, 326> kManifest = {{
    {"007firstlight", Tier::eSixX}, // 007 First Light
    {"171", Tier::eSixX}, // 171
    {"3dmark", Tier::eFourX}, // 3D Mark
    {"83", Tier::eSixX}, // 83
    {"adifficultgameaboutrollingreuprise", Tier::eSixX}, // A Difficult Game About ROLLING - ReUpRise
    {"aila", Tier::eSixX}, // A.I.L.A
    {"aion2", Tier::eFourX}, // AION2
    {"akimbot", Tier::eSixX}, // Akimbot
    {"alanwake2", Tier::eSixX}, // Alan Wake 2
    {"alienrogueincursionpartoneevolvededition", Tier::eSixX}, // Alien: Rogue Incursion - Part One: Evolved Edition
    {"ambulancelifeaparamedicsimulator", Tier::eSixX}, // Ambulance Life: A Paramedic Simulator
    {"aquietplacetheroadahead", Tier::eSixX}, // A Quiet Place: The Road Ahead
    {"arcraiders", Tier::eFourX}, // ARC Raiders
    {"arenabreakoutinfinite", Tier::eFourX}, // Arena Breakout: Infinite
    {"arknightsendfield", Tier::eFourX}, // Arknights: Endfield
    {"arksurvivalascended", Tier::eSixX}, // ARK: Survival Ascended
    {"ashesofcreation", Tier::eSixX}, // Ashes Of Creation
    {"assassinscreedblackflagresynced", Tier::eSixX}, // Assassin's Creed Black Flag Resynced
    {"assassinscreedshadows", Tier::eFourX}, // Assassin's Creed Shadows
    {"assettocorsarally", Tier::eSixX}, // Assetto Corsa Rally
    {"astrometica", Tier::eSixX}, // Astrometica
    {"atomicheart", Tier::eSixX}, // Atomic Heart
    {"auntfatima", Tier::eSixX}, // Aunt Fatima - خالة فاطمة
    {"automateitfactorypuzzle", Tier::eSixX}, // Automate It: Factory Puzzle
    {"avatarfrontiersofpandora", Tier::eFourX}, // Avatar: Frontiers of Pandora
    {"avowed", Tier::eSixX}, // Avowed
    {"backroomsescapetogether", Tier::eSixX}, // Backrooms: Escape Together
    {"banishersghostsofneweden", Tier::eSixX}, // Banishers: Ghosts of New Eden
    {"bastide", Tier::eSixX}, // Bastide
    {"battlefield6", Tier::eFourX}, // Battlefield 6
    {"bearsinspace", Tier::eSixX}, // Bears In Space
    {"beastfalseprophet", Tier::eSixX}, // BEAST: False Prophet
    {"bellwright", Tier::eSixX}, // Bellwright
    {"beyondhanwell", Tier::eSixX}, // Beyond Hanwell
    {"blackmythwukong", Tier::eFourX}, // Black Myth: Wukong
    {"bladesoffire", Tier::eSixX}, // Blades of Fire
    {"borderlands4", Tier::eSixX}, // Borderlands 4
    {"breathedge2", Tier::eSixX}, // Breathedge 2
    {"brickadia", Tier::eFourX}, // Brickadia
    {"bumrevenge", Tier::eSixX}, // Bum: Revenge
    {"busbound", Tier::eSixX}, // Bus Bound
    {"callofdutyblackops7", Tier::eFourX}, // Call of Duty: Black Ops 7
    {"carpathiansurvival", Tier::eSixX}, // Carpathian Survival
    {"casacaballero", Tier::eSixX}, // Casa Caballero
    {"chainedbackrooms", Tier::eFourX}, // Chained Backrooms
    {"chernobylite2exclusionzone", Tier::eSixX}, // Chernobylite 2: Exclusion Zone
    {"clairobscurexpedition33", Tier::eSixX}, // Clair Obscur: Expedition 33
    {"codealkonostawakeningofevil", Tier::eSixX}, // Code Alkonost: Awakening of Evil
    {"conanexilesenhanced", Tier::eSixX}, // Conan Exiles Enhanced
    {"corsaircove", Tier::eSixX}, // Corsair Cove
    {"crimsondesert", Tier::eSixX}, // Crimson Desert
    {"crimsonmoon", Tier::eFourX}, // Crimson Moon
    {"cronosthenewdawn", Tier::eSixX}, // Cronos: The New Dawn
    {"crownsimulatorroyallifesimulation", Tier::eFourX}, // Crown Simulator - Royal Life Simulation
    {"cthulhuthecosmicabyss", Tier::eSixX}, // Cthulhu: The Cosmic Abyss
    {"curseddeal", Tier::eSixX}, // Cursed Deal
    {"cyberpunk2077", Tier::eSixX}, // Cyberpunk 2077
    {"d5render", Tier::eSixX}, // D5 Render
    {"davyxjones", Tier::eFourX}, // DAVY x JONES
    {"dawnofdefiance", Tier::eSixX}, // Dawn of Defiance
    {"deadcamanalogsurvivalhorror", Tier::eSixX}, // DEADCAM | ANALOG • SURVIVAL • HORROR
    {"deadtake", Tier::eSixX}, // Dead Take
    {"deadzonerogue", Tier::eSixX}, // Deadzone: Rogue
    {"deathground", Tier::eSixX}, // Deathground
    {"deathrelives", Tier::eSixX}, // Death Relives
    {"deathstranding2onthebeach", Tier::eFourX}, // Death Stranding 2: On The Beach
    {"deceit2", Tier::eSixX}, // Deceit 2
    {"deedleedoocarkour", Tier::eSixX}, // Deedlee Doo! Carkour!
    {"deeprockgalactic", Tier::eSixX}, // Deep Rock Galactic
    {"deeprockgalacticroguecore", Tier::eSixX}, // Deep Rock Galactic: Rogue Core
    {"deliverusmars", Tier::eSixX}, // Deliver Us Mars
    {"deltaforceblackhawkdowncampaign", Tier::eFourX}, // Delta Force: Black Hawk Down Campaign
    {"demonologist", Tier::eSixX}, // Demonologist
    {"desordreapuzzlegameadventure", Tier::eSixX}, // DESORDRE: A Puzzle Game Adventure
    {"desyncedautonomouscolonysimulator", Tier::eSixX}, // Desynced: Autonomous Colony Simulator
    {"diabloiv", Tier::eSixX}, // Diablo IV
    {"directcontact", Tier::eSixX}, // DIRECT CONTACT
    {"directive8020", Tier::eSixX}, // Directive 8020
    {"doomthedarkages", Tier::eSixX}, // DOOM: The Dark Ages
    {"dragonagetheveilguard", Tier::eSixX}, // Dragon Age: The Veilguard
    {"dragonkinthebanished", Tier::eSixX}, // Dragonkin: The Banished
    {"dreadzone", Tier::eSixX}, // DREADZONE
    {"duckside", Tier::eSixX}, // DUCKSIDE
    {"duetnightabyss", Tier::eSixX}, // Duet Night Abyss
    {"duneawakening", Tier::eSixX}, // Dune: Awakening
    {"dungeonborne", Tier::eSixX}, // Dungeonborne
    {"dyinglight2stayhuman", Tier::eFourX}, // Dying Light 2 Stay Human
    {"dyinglightthebeast", Tier::eFourX}, // Dying Light: The Beast
    {"dynastywarriorsorigins", Tier::eSixX}, // DYNASTY WARRIORS: ORIGINS
    {"echoesoftheendenhancededition", Tier::eSixX}, // Echoes of the End: Enhanced Edition
    {"eldegardeformerlylegacysteelsorcery", Tier::eSixX}, // Eldegarde (Formerly 'Legacy: Steel & Sorcery')
    {"electronicmarketsimulator", Tier::eSixX}, // Electronic Market Simulator
    {"empireoftheants", Tier::eSixX}, // Empire of the Ants
    {"empulse", Tier::eSixX}, // Empulse
    {"empyreal", Tier::eSixX}, // Empyreal
    {"enlisted", Tier::eFourX}, // Enlisted
    {"enotriathelastsong", Tier::eSixX}, // Enotria: The Last Song
    {"entitytheblackday", Tier::eSixX}, // ENTITY: THE BLACK DAY
    {"epicgamestwinmotionue", Tier::eSixX}, // Epic Games' Twinmotion-UE
    {"eternalstrands", Tier::eSixX}, // Eternal Strands
    {"evefrontier", Tier::eSixX}, // EVE Frontier
    {"eveonline", Tier::eSixX}, // EVE Online
    {"everspace2", Tier::eSixX}, // Everspace 2
    {"evotinction", Tier::eSixX}, // EVOTINCTION
    {"exfil", Tier::eSixX}, // EXFIL
    {"f125", Tier::eFourX}, // F1 25
    {"farfarwest", Tier::eSixX}, // Far Far West
    {"farlight84", Tier::eFourX}, // Farlight 84
    {"farmingsimulator25", Tier::eSixX}, // Farming Simulator 25
    {"fatekeeper", Tier::eSixX}, // Fatekeeper
    {"fbcfirebreak", Tier::eSixX}, // FBC: Firebreak
    {"finalfantasyviirebirth", Tier::eFourX}, // FINAL FANTASY VII REBIRTH
    {"finalfantasyxvi", Tier::eSixX}, // FINAL FANTASY XVI
    {"finnishcottagesimulator", Tier::eFourX}, // Finnish Cottage Simulator
    {"firefightingsimulatorignite", Tier::eSixX}, // Firefighting Simulator: Ignite
    {"flintlockthesiegeofdawn", Tier::eSixX}, // Flintlock: The Siege of Dawn
    {"foreverskies", Tier::eSixX}, // Forever Skies
    {"fortsolis", Tier::eSixX}, // Fort Solis
    {"forzahorizon6", Tier::eSixX}, // Forza Horizon 6
    {"fragpunk", Tier::eSixX}, // FragPunk
    {"frostpunk2", Tier::eSixX}, // Frostpunk 2
    {"ghostrunner2", Tier::eSixX}, // Ghostrunner 2
    {"goals", Tier::eFourX}, // GOALS
    {"godofwarragnark", Tier::eSixX}, // God of War Ragnarök
    {"goofygorillas", Tier::eSixX}, // Goofy Gorillas
    {"gothic1remake", Tier::eSixX}, // Gothic 1 Remake
    {"grandtheftautovenhanced", Tier::eFourX}, // Grand Theft Auto V Enhanced
    {"grayzonewarfare", Tier::eSixX}, // Gray Zone Warfare
    {"greedland", Tier::eSixX}, // Greedland
    {"groundbranch", Tier::eSixX}, // Ground Branch
    {"halflife2rtxdemo", Tier::eFourX}, // Half-Life 2 RTX Demo
    {"halfsword", Tier::eSixX}, // Half Sword
    {"halocampaignevolved", Tier::eSixX}, // Halo: Campaign Evolved
    {"hellbladeiienhancedsenuassaga", Tier::eSixX}, // Hellblade II Enhanced: Senua's Saga
    {"hellisus", Tier::eSixX}, // Hell Is Us
    {"hellletloosevietnam", Tier::eSixX}, // Hell Let Loose: Vietnam
    {"heroickingdomorigins", Tier::eSixX}, // Heroic Kingdom: Origins
    {"highonlife2", Tier::eSixX}, // High On Life 2
    {"hitmanworldofassassination", Tier::eSixX}, // HITMAN World of Assassination
    {"hogwartslegacy", Tier::eSixX}, // Hogwarts Legacy
    {"iamjesuschrist", Tier::eSixX}, // I Am Jesus Christ
    {"icarus", Tier::eSixX}, // ICARUS
    {"immortalsofaveum", Tier::eSixX}, // Immortals of Aveum
    {"indianajonesandthegreatcircle", Tier::eSixX}, // Indiana Jones and the Great Circle
    {"industria2", Tier::eSixX}, // INDUSTRIA 2
    {"industrygiant40", Tier::eSixX}, // Industry Giant 4.0
    {"infinitynikki", Tier::eFourX}, // Infinity Nikki
    {"inzoi", Tier::eSixX}, // inZOI
    {"islesofyore", Tier::eSixX}, // Isles of Yore
    {"jdmjapanesedriftmaster", Tier::eFourX}, // JDM: Japanese Drift Master
    {"johncarpenterstoxiccommando", Tier::eFourX}, // John Carpenter's Toxic Commando
    {"jurassicworldevolution3", Tier::eFourX}, // Jurassic World Evolution 3
    {"jusant", Tier::eSixX}, // Jusant
    {"justiceswordofjustice", Tier::eFourX}, // Justice / Sword of Justice
    {"jx3onlinertxversion", Tier::eSixX}, // JX3 Online RTX Version
    {"karmathedarkworld", Tier::eSixX}, // KARMA: The Dark World
    {"keeper", Tier::eSixX}, // Keeper
    {"killingfloor3", Tier::eSixX}, // Killing Floor 3
    {"kristala", Tier::eSixX}, // Kristala
    {"layersoffear", Tier::eSixX}, // Layers of Fear
    {"layoftheland", Tier::eFourX}, // Lay of the Land
    {"legendofymir", Tier::eFourX}, // Legend of Ymir
    {"legobatmanlegacyofthedarkknight", Tier::eSixX}, // LEGO Batman: Legacy of the Dark Knight
    {"legohorizonadventures", Tier::eSixX}, // LEGO Horizon Adventures
    {"levelzeroextraction", Tier::eSixX}, // Level Zero: Extraction
    {"liminalcore", Tier::eSixX}, // Liminalcore
    {"littlenightmaresiii", Tier::eSixX}, // Little Nightmares III
    {"lordsofthefallen", Tier::eSixX}, // Lords of the Fallen
    {"lostrecordsbloomrage", Tier::eSixX}, // Lost Records: Bloom & Rage
    {"lostsoulaside", Tier::eSixX}, // Lost Soul Aside
    {"luminary", Tier::eFourX}, // Luminary
    {"luto", Tier::eFourX}, // Luto
    {"mafiatheoldcountry", Tier::eSixX}, // Mafia: The Old Country
    {"manorlords", Tier::eSixX}, // Manor Lords
    {"marvelrivals", Tier::eFourX}, // Marvel Rivals
    {"marvelsspiderman2", Tier::eFourX}, // Marvel's Spider-Man 2
    {"mavrixbymattjones", Tier::eSixX}, // MAVRIX By Matt Jones
    {"mechabreak", Tier::eFourX}, // Mecha BREAK
    {"mechwarrior5clans", Tier::eSixX}, // MechWarrior 5: Clans
    {"metaleden", Tier::eSixX}, // Metal Eden
    {"microsoftflightsimulator202040thanniversaryedition", Tier::eSixX}, // Microsoft Flight Simulator (2020) 40th Anniversary Edition
    {"microsoftflightsimulator2024", Tier::eSixX}, // Microsoft Flight Simulator 2024
    {"mindseye", Tier::eSixX}, // MindsEye
    {"mistfallhunter", Tier::eSixX}, // Mistfall Hunter
    {"mongilstardive", Tier::eFourX}, // MONGIL: STAR DIVE
    {"monsterhunterwilds", Tier::eFourX}, // Monster Hunter Wilds
    {"mortalonline2", Tier::eSixX}, // Mortal Online 2
    {"mortalshellii", Tier::eSixX}, // Mortal Shell II
    {"mothermachine", Tier::eSixX}, // Mother Machine
    {"narakabladepoint", Tier::eFourX}, // NARAKA: BLADEPOINT
    {"nba2k27", Tier::eSixX}, // NBA 2K27
    {"needforspeedunbound", Tier::eSixX}, // Need For Speed Unbound
    {"newworldaeternum", Tier::eSixX}, // New World: Aeternum
    {"nightingale", Tier::eSixX}, // Nightingale
    {"nightofthedead", Tier::eSixX}, // Night of the Dead
    {"nightproject", Tier::eSixX}, // Night Project
    {"ninjagaiden2black", Tier::eSixX}, // NINJA GAIDEN 2 Black
    {"nioh3", Tier::eSixX}, // Nioh 3
    {"noblelegacy", Tier::eSixX}, // Noble Legacy
    {"nomanssky", Tier::eSixX}, // No Man's Sky
    {"nomoreroominhell2", Tier::eSixX}, // No More Room In Hell 2
    {"norseoathofblood", Tier::eSixX}, // NORSE: Oath of Blood
    {"nowherenear", Tier::eSixX}, // Nowhere Near
    {"ntenevernesstoeverness", Tier::eFourX}, // NTE (Neverness to Everness)
    {"nvidiartxremix", Tier::eFourX}, // NVIDIA RTX Remix
    {"offthegrid", Tier::eSixX}, // Off The Grid
    {"oncehuman", Tier::eFourX}, // Once Human
    {"onimushawayofthesword", Tier::eFourX}, // Onimusha: Way of the Sword
    {"otherplane", Tier::eSixX}, // Otherplane
    {"outbreakshadesofhorror", Tier::eSixX}, // Outbreak: Shades of Horror
    {"outpostinfinitysiege", Tier::eSixX}, // Outpost: Infinity Siege
    {"palworld", Tier::eSixX}, // Palworld
    {"parkstudio", Tier::eSixX}, // Park Studio
    {"paxdei", Tier::eSixX}, // Pax Dei
    {"payday3", Tier::eSixX}, // PAYDAY 3
    {"pizzabandit", Tier::eSixX}, // Pizza Bandit
    {"portalwithrtx", Tier::eFourX}, // Portal with RTX
    {"postal4noregerts", Tier::eSixX}, // Postal 4: No Regerts
    {"pragmata", Tier::eFourX}, // PRAGMATA
    {"predecessor", Tier::eFourX}, // Predecessor
    {"projectmotorracing", Tier::eSixX}, // Project Motor Racing
    {"prologuegowayback", Tier::eSixX}, // Prologue: Go Wayback!
    {"readyornot", Tier::eSixX}, // Ready or Not
    {"reallife", Tier::eFourX}, // Real Life
    {"reaperactual", Tier::eSixX}, // Reaper Actual
    {"redfall", Tier::eSixX}, // Redfall
    {"rematch", Tier::eSixX}, // REMATCH
    {"remnantii", Tier::eSixX}, // Remnant II
    {"residentevilrequiem", Tier::eFourX}, // Resident Evil Requiem
    {"resonanceaplaguetalelegacy", Tier::eSixX}, // Resonance: A Plague Tale Legacy
    {"returntocampus", Tier::eSixX}, // Return to Campus
    {"riseoftheronin", Tier::eSixX}, // Rise of the Ronin
    {"roadcraft", Tier::eFourX}, // RoadCraft
    {"robocoproguecity", Tier::eSixX}, // RoboCop: Rogue City
    {"robocoproguecityunfinishedbusiness", Tier::eSixX}, // RoboCop: Rogue City - Unfinished Business
    {"rocketsquadinfinity", Tier::eSixX}, // Rocket Squad: Infinity
    {"runefactoryguardiansofazuma", Tier::eSixX}, // Rune Factory: Guardians of Azuma
    {"runescapedragonwilds", Tier::eSixX}, // RuneScape: Dragonwilds
    {"samson", Tier::eFourX}, // Samson
    {"satisfactory", Tier::eSixX}, // Satisfactory
    {"screamer", Tier::eSixX}, // Screamer
    {"seafarertheshipsim", Tier::eSixX}, // Seafarer: The Ship Sim
    {"sengokudynasty", Tier::eSixX}, // Sengoku Dynasty
    {"serum", Tier::eSixX}, // Serum
    {"silenthill2", Tier::eSixX}, // SILENT HILL 2
    {"simulakros", Tier::eSixX}, // Simulakros
    {"skyethemistyisle", Tier::eSixX}, // Skye: The Misty Isle
    {"slenderthearrival", Tier::eSixX}, // Slender: The Arrival
    {"smallspaces", Tier::eSixX}, // Small Spaces
    {"smite2", Tier::eSixX}, // SMITE 2
    {"sophonce", Tier::eSixX}, // Sophonce
    {"spiritofthenorth2", Tier::eSixX}, // Spirit of the North 2
    {"splitgatearenareloaded", Tier::eSixX}, // Splitgate: Arena Reloaded
    {"spongebobsquarepantstitansofthetide", Tier::eSixX}, // SpongeBob SquarePants: Titans of the Tide
    {"squad", Tier::eSixX}, // Squad
    {"stalker2heartofchornobyl", Tier::eSixX}, // S.T.A.L.K.E.R. 2: Heart of Chornobyl
    {"starminer", Tier::eSixX}, // Starminer
    {"starrupture", Tier::eSixX}, // StarRupture
    {"starshiptroopersextermination", Tier::eSixX}, // Starship Troopers: Extermination
    {"startrekvoyageracrosstheunknown", Tier::eSixX}, // Star Trek: Voyager - Across The Unknown
    {"starwarsjedisurvivor", Tier::eSixX}, // Star Wars Jedi: Survivor
    {"starwarsoutlaws", Tier::eFourX}, // Star Wars Outlaws
    {"starwarszerocompany", Tier::eSixX}, // STAR WARS Zero Company
    {"steelseed", Tier::eSixX}, // Steel Seed
    {"stellarblade", Tier::eSixX}, // Stellar Blade
    {"stillwakesthedeep", Tier::eSixX}, // Still Wakes The Deep
    {"storagehuntersimulator", Tier::eSixX}, // Storage Hunter Simulator
    {"stormgate", Tier::eSixX}, // Stormgate
    {"storrorparkourpro", Tier::eSixX}, // STORROR Parkour Pro
    {"stygianoutergods", Tier::eSixX}, // Stygian: Outer Gods
    {"styxbladesofgreed", Tier::eSixX}, // Styx: Blades of Greed
    {"subliminal", Tier::eSixX}, // Subliminal
    {"subnautica2", Tier::eSixX}, // Subnautica 2
    {"supermovesworldofparkour", Tier::eSixX}, // Supermoves: World of Parkour
    {"supraworld", Tier::eSixX}, // Supraworld
    {"tankhead", Tier::eSixX}, // Tankhead
    {"tempestrising", Tier::eSixX}, // Tempest Rising
    {"testdriveunlimitedsolarcrown", Tier::eSixX}, // Test Drive Unlimited Solar Crown
    {"thealters", Tier::eSixX}, // The Alters
    {"theaxisunseen", Tier::eSixX}, // The Axis Unseen
    {"thebackroomslosttape", Tier::eSixX}, // The Backrooms: Lost Tape
    {"theblackpool", Tier::eSixX}, // The Black Pool
    {"theblackpoolarenasurvivors", Tier::eSixX}, // The Black Pool: Arena Survivors
    {"thebus", Tier::eSixX}, // The Bus
    {"thecastingoffrankstone", Tier::eSixX}, // The Casting of Frank Stone
    {"theelderscrollsivoblivionremastered", Tier::eSixX}, // The Elder Scrolls IV: Oblivion Remastered
    {"thefinals", Tier::eFourX}, // THE FINALS
    {"thefirstberserkerkhazan", Tier::eSixX}, // The First Berserker: Khazan
    {"thefirstdescendant", Tier::eSixX}, // The First Descendant
    {"thegoldriverproject", Tier::eSixX}, // The Gold River Project
    {"thelastcaretaker", Tier::eSixX}, // The Last Caretaker
    {"thelastofuspartiiremastered", Tier::eFourX}, // The Last of Us Part II Remastered
    {"themoundomenofcthulhu", Tier::eSixX}, // The Mound: Omen of Cthulhu
    {"theoccultist", Tier::eFourX}, // The Occultist
    {"theouterworlds2", Tier::eFourX}, // The Outer Worlds 2
    {"thesinkingcity2", Tier::eSixX}, // The Sinking City 2
    {"thetalosprinciplereawakened", Tier::eFourX}, // The Talos Principle: Reawakened
    {"thethaumaturge", Tier::eSixX}, // The Thaumaturge
    {"throneandliberty", Tier::eSixX}, // THRONE AND LIBERTY
    {"titanquestii", Tier::eSixX}, // Titan Quest II
    {"tokyoxtremeracer", Tier::eSixX}, // Tokyo Xtreme Racer
    {"torquedrift2", Tier::eSixX}, // Torque Drift 2
    {"tribes3rivals", Tier::eSixX}, // TRIBES 3: Rivals
    {"untildawn", Tier::eSixX}, // Until Dawn
    {"vampiresbloodlordrising", Tier::eSixX}, // Vampires: Bloodlord Rising
    {"vampirethemasqueradebloodlines2", Tier::eSixX}, // Vampire: The Masquerade - Bloodlines 2
    {"vectorstrike", Tier::eSixX}, // Vector Strike
    {"wackywest", Tier::eSixX}, // Wacky West
    {"warhammer40000darktide", Tier::eFourX}, // Warhammer 40,000: Darktide
    {"warhammer40000spacemarine2", Tier::eFourX}, // Warhammer 40,000: Space Marine 2
    {"warthunder", Tier::eFourX}, // War Thunder
    {"wherewindsmeet", Tier::eFourX}, // Where Winds Meet
    {"whydonttheylaugh", Tier::eSixX}, // Why Don't They Laugh?
    {"wildassault", Tier::eSixX}, // Wild Assault
    {"windrose", Tier::eSixX}, // Windrose
    {"wintersurvival", Tier::eSixX}, // Winter Survival
    {"witchfire", Tier::eSixX}, // Witchfire
    {"worldofjadedynasty", Tier::eSixX}, // World of Jade Dynasty
    {"worldoftanksheat", Tier::eSixX}, // World of Tanks: HEAT
    {"wreckfest2", Tier::eFourX}, // Wreckfest 2
    {"wuchangfallenfeathers", Tier::eSixX}, // WUCHANG: Fallen Feathers
    {"wutheringwaves", Tier::eSixX}, // Wuthering Waves
    {"x4foundations", Tier::eFourX}, // X4: Foundations
    {"yakuzakiwami2", Tier::eSixX}, // Yakuza Kiwami 2
    {"yakuzakiwami3darkties", Tier::eSixX}, // Yakuza Kiwami 3 & Dark Ties
    {"zombiecity", Tier::eSixX}, // Zombie City
}};
