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

```dot
digraph rmg_old {
  rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#f7f7f7", fontname="Helvetica"];

  start [label="generate() start"];
  end [label="generate() end\nwall: 13549.73 ms", fillcolor="#e8f5e9"];

  WaterAdopter [label="WaterAdopter\n4954 ms / 19 / 260.7"];
  TownPlacer [label="TownPlacer\n3039 ms / 18 / 168.8"];
  TerrainPainter [label="TerrainPainter\n21569 ms / 19 / 1135.2"];
  WaterProxy [label="WaterProxy\n260 ms / 1 / 260.0"];
  ConnectionsPlacer [label="ConnectionsPlacer\n9932 ms / 18 / 551.8"];
  MinePlacer [label="MinePlacer\n28 ms / 18 / 1.6"];
  ObjectPlacer [label="ObjectPlacer\n0 ms / 18 / 0.0"];
  ObjectManager [label="ObjectManager\n457037 ms / 19 / 24054.6", fillcolor="#ffe0b2"];
  RoadPlacer [label="RoadPlacer\n3389 ms / 18 / 188.3"];
  TreasurePlacer [label="TreasurePlacer\n157813 ms / 19 / 8305.9", fillcolor="#ffe0b2"];
  ObjectDistributor [label="ObjectDistributor\n8 ms / 1 / 8.0"];
  PrisonHeroPlacer [label="PrisonHeroPlacer\n0 ms / 1 / 0.0"];
  WaterRoutes [label="WaterRoutes\n412 ms / 1 / 412.0"];
  RockPlacer [label="RockPlacer\n253 ms / 9 / 28.1"];
  RockFiller [label="RockFiller\n1999 ms / 1 / 1999.0"];
  ObstaclePlacer [label="ObstaclePlacer\n19117 ms / 19 / 1006.2"];
  RiverPlacer [label="RiverPlacer\n5890 ms / 18 / 327.2"];
  QuestArtifactPlacer [label="QuestArtifactPlacer\n314 ms / 18 / 17.4"];

  start -> WaterAdopter;
  start -> PrisonHeroPlacer;
  start -> WaterRoutes;

  WaterAdopter -> TownPlacer;
  WaterAdopter -> TerrainPainter;
  WaterAdopter -> ConnectionsPlacer;
  WaterAdopter -> ObjectManager;
  WaterAdopter -> WaterProxy;
  WaterAdopter -> TreasurePlacer;

  TownPlacer -> TerrainPainter;
  TownPlacer -> ConnectionsPlacer;
  TownPlacer -> MinePlacer;
  TownPlacer -> ObjectPlacer;
  TownPlacer -> ObjectManager;
  TownPlacer -> RoadPlacer;
  TownPlacer -> WaterProxy;

  TerrainPainter -> WaterProxy;
  TerrainPainter -> ConnectionsPlacer;
  TerrainPainter -> ObjectManager;
  TerrainPainter -> ObjectDistributor;

  WaterProxy -> ConnectionsPlacer;
  WaterProxy -> ObjectManager;
  WaterProxy -> TreasurePlacer;
  WaterProxy -> ObstaclePlacer;
  WaterProxy -> RiverPlacer;

  ConnectionsPlacer -> MinePlacer;
  ConnectionsPlacer -> ObjectPlacer;
  ConnectionsPlacer -> ObjectManager;
  ConnectionsPlacer -> RoadPlacer;
  ConnectionsPlacer -> TreasurePlacer;

  MinePlacer -> ObjectManager;
  MinePlacer -> RoadPlacer;
  ObjectPlacer -> ObjectManager;
  ObjectPlacer -> RoadPlacer;
  ObjectManager -> RoadPlacer;
  ObjectManager -> TreasurePlacer;
  ObjectManager -> ObstaclePlacer;
  ObjectManager -> RiverPlacer;

  ObjectDistributor -> TreasurePlacer;
  PrisonHeroPlacer -> TreasurePlacer;

  RoadPlacer -> TreasurePlacer;
  RoadPlacer -> ObstaclePlacer;
  RoadPlacer -> RockPlacer;

  TreasurePlacer -> ObstaclePlacer;
  TreasurePlacer -> QuestArtifactPlacer;

  RockPlacer -> RockFiller;
  RockFiller -> ObstaclePlacer;
  WaterRoutes -> ObstaclePlacer;
  ObstaclePlacer -> RiverPlacer;

  QuestArtifactPlacer -> end;
  RiverPlacer -> end;
}
```

## New pipeline (`10959297f...`)

```dot
digraph rmg_new {
  rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#f7f7f7", fontname="Helvetica"];

  start [label="generate() start"];
  end [label="generate() end\nwall: 15466.75 ms", fillcolor="#e8f5e9"];

  WaterAdopter [label="WaterAdopter\n2361 ms / 19 / 124.3"];
  TownPlacer [label="TownPlacer\n58 ms / 18 / 3.2"];
  TerrainPainter [label="TerrainPainter\n718 ms / 19 / 37.8"];
  WaterProxy [label="WaterProxy\n287 ms / 1 / 287.0"];
  ConnectionsPlacer [label="ConnectionsPlacer\n114 ms / 18 / 6.3"];
  MinePlacer [label="MinePlacer\n56 ms / 18 / 3.1"];
  ObjectPlacer [label="ObjectPlacer\n0 ms / 18 / 0.0"];
  ObjectManager [label="ObjectManager\n19443 ms / 19 / 1023.3", fillcolor="#ffe0b2"];
  RoadPlacer [label="RoadPlacer\n3622 ms / 18 / 201.2"];
  TreasurePlacer [label="TreasurePlacer\n62849 ms / 19 / 3307.8", fillcolor="#ffe0b2"];
  ObjectDistributor [label="ObjectDistributor\n6 ms / 1 / 6.0"];
  PrisonHeroPlacer [label="PrisonHeroPlacer\n0 ms / 1 / 0.0"];
  WaterRoutes [label="WaterRoutes\n179 ms / 1 / 179.0"];
  RockPlacer [label="RockPlacer\n142 ms / 9 / 15.8"];
  RockFiller [label="RockFiller\n820 ms / 1 / 820.0"];
  ObstaclePlacer [label="ObstaclePlacer\n15562 ms / 19 / 819.1"];
  RiverPlacer [label="RiverPlacer\n1855 ms / 18 / 103.1"];
  QuestArtifactPlacer [label="QuestArtifactPlacer\n7 ms / 18 / 0.4"];

  start -> WaterAdopter;
  start -> PrisonHeroPlacer;
  start -> WaterRoutes;

  WaterAdopter -> TownPlacer;
  WaterAdopter -> TerrainPainter;
  WaterAdopter -> ConnectionsPlacer;
  WaterAdopter -> ObjectManager;
  WaterAdopter -> WaterProxy;
  WaterAdopter -> TreasurePlacer;

  TownPlacer -> TerrainPainter;
  TownPlacer -> ConnectionsPlacer;
  TownPlacer -> MinePlacer;
  TownPlacer -> ObjectPlacer;
  TownPlacer -> ObjectManager;
  TownPlacer -> RoadPlacer;
  TownPlacer -> WaterProxy;

  TerrainPainter -> WaterProxy;
  TerrainPainter -> ConnectionsPlacer;
  TerrainPainter -> ObjectManager;
  TerrainPainter -> ObjectDistributor;

  WaterProxy -> ConnectionsPlacer;
  WaterProxy -> ObjectManager;
  WaterProxy -> TreasurePlacer;
  WaterProxy -> ObstaclePlacer;
  WaterProxy -> RiverPlacer;

  ConnectionsPlacer -> MinePlacer;
  ConnectionsPlacer -> ObjectPlacer;
  ConnectionsPlacer -> ObjectManager;
  ConnectionsPlacer -> RoadPlacer;
  ConnectionsPlacer -> TreasurePlacer;

  MinePlacer -> ObjectManager;
  MinePlacer -> RoadPlacer;
  ObjectPlacer -> ObjectManager;
  ObjectPlacer -> RoadPlacer;
  ObjectManager -> RoadPlacer;
  ObjectManager -> TreasurePlacer;
  ObjectManager -> ObstaclePlacer;
  ObjectManager -> RiverPlacer;

  ObjectDistributor -> TreasurePlacer;
  PrisonHeroPlacer -> TreasurePlacer;

  RoadPlacer -> TreasurePlacer;
  RoadPlacer -> ObstaclePlacer;
  RoadPlacer -> RockPlacer;

  TreasurePlacer -> ObstaclePlacer;
  TreasurePlacer -> QuestArtifactPlacer;

  RockPlacer -> RockFiller;
  RockFiller -> ObstaclePlacer;
  WaterRoutes -> ObstaclePlacer;
  ObstaclePlacer -> RiverPlacer;

  QuestArtifactPlacer -> end;
  RiverPlacer -> end;
}
```
