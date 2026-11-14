# Known limitations

* Terrain shape is tuned for the default 1024 map; smaller maps keep the same
  number of features and come out steeper.
* The water is a single layer: no water under water, no breaking waves.
* The procedural fallback textures are placeholders.
* The dam stamp's preview shows the dragged line only, not the extension
  into the banks.
* Groundwater does not flow: moisture stays where it is until it dries.
* Small objects (up to about 1.2 m) buried in silt are gone for good: out of
  the physics, the navigation and the map.
* The interface is for desktop (mouse and keyboard); touch will need its own.
* Changing the anisotropy does not reach the objects' textures until the
  next start.
