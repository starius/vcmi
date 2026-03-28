# RMG pipeline latencies (Clash of Dragons, 252x252x2)

Scenario used for both diagrams:
- template: `vcmi:Clash of Dragons`
- map: `252x252x2`
- scheduler: `parallel`
- workers: `16`
- seed: `1`
- timestamp: `1742174175`
- benchmark args: `--warmup 0 --runs 1`

Commits:
- old: `f88934907797d3b84421efca11f8c79021ccf8d8`
- new: `10959297f8a0004a6db66ebe5b921a43c3111551`

Latency labels on nodes are cumulative per-node execution time from this run:
`total ms / calls / avg ms`. This is not wall-clock critical-path time; nodes run in
parallel and overlap.

Wall-clock benchmark result:
- old: `13549.73 ms`
- new: `15466.75 ms`

## Old pipeline (`f889349...`)

```mermaid
flowchart LR
  startNode["generate() start"]
  endNode["generate() end<br/>wall: 13549.73 ms"]

  WaterAdopter["WaterAdopter<br/>4954 ms / 19 / 260.7"]
  TownPlacer["TownPlacer<br/>3039 ms / 18 / 168.8"]
  TerrainPainter["TerrainPainter<br/>21569 ms / 19 / 1135.2"]
  WaterProxy["WaterProxy<br/>260 ms / 1 / 260.0"]
  ConnectionsPlacer["ConnectionsPlacer<br/>9932 ms / 18 / 551.8"]
  MinePlacer["MinePlacer<br/>28 ms / 18 / 1.6"]
  ObjectPlacer["ObjectPlacer<br/>0 ms / 18 / 0.0"]
  ObjectManager["ObjectManager<br/>457037 ms / 19 / 24054.6"]
  RoadPlacer["RoadPlacer<br/>3389 ms / 18 / 188.3"]
  TreasurePlacer["TreasurePlacer<br/>157813 ms / 19 / 8305.9"]
  ObjectDistributor["ObjectDistributor<br/>8 ms / 1 / 8.0"]
  PrisonHeroPlacer["PrisonHeroPlacer<br/>0 ms / 1 / 0.0"]
  WaterRoutes["WaterRoutes<br/>412 ms / 1 / 412.0"]
  RockPlacer["RockPlacer<br/>253 ms / 9 / 28.1"]
  RockFiller["RockFiller<br/>1999 ms / 1 / 1999.0"]
  ObstaclePlacer["ObstaclePlacer<br/>19117 ms / 19 / 1006.2"]
  RiverPlacer["RiverPlacer<br/>5890 ms / 18 / 327.2"]
  QuestArtifactPlacer["QuestArtifactPlacer<br/>314 ms / 18 / 17.4"]

  startNode --> WaterAdopter
  startNode --> PrisonHeroPlacer
  startNode --> WaterRoutes

  WaterAdopter --> TownPlacer
  WaterAdopter --> TerrainPainter
  WaterAdopter --> ConnectionsPlacer
  WaterAdopter --> ObjectManager
  WaterAdopter --> WaterProxy
  WaterAdopter --> TreasurePlacer

  TownPlacer --> TerrainPainter
  TownPlacer --> ConnectionsPlacer
  TownPlacer --> MinePlacer
  TownPlacer --> ObjectPlacer
  TownPlacer --> ObjectManager
  TownPlacer --> RoadPlacer
  TownPlacer --> WaterProxy

  TerrainPainter --> WaterProxy
  TerrainPainter --> ConnectionsPlacer
  TerrainPainter --> ObjectManager
  TerrainPainter --> ObjectDistributor

  WaterProxy --> ConnectionsPlacer
  WaterProxy --> ObjectManager
  WaterProxy --> TreasurePlacer
  WaterProxy --> ObstaclePlacer
  WaterProxy --> RiverPlacer

  ConnectionsPlacer --> MinePlacer
  ConnectionsPlacer --> ObjectPlacer
  ConnectionsPlacer --> ObjectManager
  ConnectionsPlacer --> RoadPlacer
  ConnectionsPlacer --> TreasurePlacer

  MinePlacer --> ObjectManager
  MinePlacer --> RoadPlacer
  ObjectPlacer --> ObjectManager
  ObjectPlacer --> RoadPlacer
  ObjectManager --> RoadPlacer
  ObjectManager --> TreasurePlacer
  ObjectManager --> ObstaclePlacer
  ObjectManager --> RiverPlacer

  ObjectDistributor --> TreasurePlacer
  PrisonHeroPlacer --> TreasurePlacer

  RoadPlacer --> TreasurePlacer
  RoadPlacer --> ObstaclePlacer
  RoadPlacer --> RockPlacer

  TreasurePlacer --> ObstaclePlacer
  TreasurePlacer --> QuestArtifactPlacer

  RockPlacer --> RockFiller
  RockFiller --> ObstaclePlacer
  WaterRoutes --> ObstaclePlacer
  ObstaclePlacer --> RiverPlacer

  QuestArtifactPlacer --> endNode
  RiverPlacer --> endNode

  classDef heavy fill:#ffe0b2,stroke:#333,stroke-width:1px;
  classDef done fill:#e8f5e9,stroke:#333,stroke-width:1px;
  class ObjectManager,TreasurePlacer heavy;
  class endNode done;
```

## New pipeline (`10959297f...`)

```mermaid
flowchart LR
  startNode["generate() start"]
  endNode["generate() end<br/>wall: 15466.75 ms"]

  WaterAdopter["WaterAdopter<br/>2361 ms / 19 / 124.3"]
  TownPlacer["TownPlacer<br/>58 ms / 18 / 3.2"]
  TerrainPainter["TerrainPainter<br/>718 ms / 19 / 37.8"]
  WaterProxy["WaterProxy<br/>287 ms / 1 / 287.0"]
  ConnectionsPlacer["ConnectionsPlacer<br/>114 ms / 18 / 6.3"]
  MinePlacer["MinePlacer<br/>56 ms / 18 / 3.1"]
  ObjectPlacer["ObjectPlacer<br/>0 ms / 18 / 0.0"]
  ObjectManager["ObjectManager<br/>19443 ms / 19 / 1023.3"]
  RoadPlacer["RoadPlacer<br/>3622 ms / 18 / 201.2"]
  TreasurePlacer["TreasurePlacer<br/>62849 ms / 19 / 3307.8"]
  ObjectDistributor["ObjectDistributor<br/>6 ms / 1 / 6.0"]
  PrisonHeroPlacer["PrisonHeroPlacer<br/>0 ms / 1 / 0.0"]
  WaterRoutes["WaterRoutes<br/>179 ms / 1 / 179.0"]
  RockPlacer["RockPlacer<br/>142 ms / 9 / 15.8"]
  RockFiller["RockFiller<br/>820 ms / 1 / 820.0"]
  ObstaclePlacer["ObstaclePlacer<br/>15562 ms / 19 / 819.1"]
  RiverPlacer["RiverPlacer<br/>1855 ms / 18 / 103.1"]
  QuestArtifactPlacer["QuestArtifactPlacer<br/>7 ms / 18 / 0.4"]

  startNode --> WaterAdopter
  startNode --> PrisonHeroPlacer
  startNode --> WaterRoutes

  WaterAdopter --> TownPlacer
  WaterAdopter --> TerrainPainter
  WaterAdopter --> ConnectionsPlacer
  WaterAdopter --> ObjectManager
  WaterAdopter --> WaterProxy
  WaterAdopter --> TreasurePlacer

  TownPlacer --> TerrainPainter
  TownPlacer --> ConnectionsPlacer
  TownPlacer --> MinePlacer
  TownPlacer --> ObjectPlacer
  TownPlacer --> ObjectManager
  TownPlacer --> RoadPlacer
  TownPlacer --> WaterProxy

  TerrainPainter --> WaterProxy
  TerrainPainter --> ConnectionsPlacer
  TerrainPainter --> ObjectManager
  TerrainPainter --> ObjectDistributor

  WaterProxy --> ConnectionsPlacer
  WaterProxy --> ObjectManager
  WaterProxy --> TreasurePlacer
  WaterProxy --> ObstaclePlacer
  WaterProxy --> RiverPlacer

  ConnectionsPlacer --> MinePlacer
  ConnectionsPlacer --> ObjectPlacer
  ConnectionsPlacer --> ObjectManager
  ConnectionsPlacer --> RoadPlacer
  ConnectionsPlacer --> TreasurePlacer

  MinePlacer --> ObjectManager
  MinePlacer --> RoadPlacer
  ObjectPlacer --> ObjectManager
  ObjectPlacer --> RoadPlacer
  ObjectManager --> RoadPlacer
  ObjectManager --> TreasurePlacer
  ObjectManager --> ObstaclePlacer
  ObjectManager --> RiverPlacer

  ObjectDistributor --> TreasurePlacer
  PrisonHeroPlacer --> TreasurePlacer

  RoadPlacer --> TreasurePlacer
  RoadPlacer --> ObstaclePlacer
  RoadPlacer --> RockPlacer

  TreasurePlacer --> ObstaclePlacer
  TreasurePlacer --> QuestArtifactPlacer

  RockPlacer --> RockFiller
  RockFiller --> ObstaclePlacer
  WaterRoutes --> ObstaclePlacer
  ObstaclePlacer --> RiverPlacer

  QuestArtifactPlacer --> endNode
  RiverPlacer --> endNode

  classDef heavy fill:#ffe0b2,stroke:#333,stroke-width:1px;
  classDef done fill:#e8f5e9,stroke:#333,stroke-width:1px;
  class ObjectManager,TreasurePlacer heavy;
  class endNode done;
```
