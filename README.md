
<!-- PROJECT LOGO -->
<br />
<div align="center">
  <a href="https://github.com/weasalcrafter/linklite-v2">
    <img src="images/logo.png" alt="Logo" width="600">
  </a>

  <h3 align="center">LinkLite V2</h3>

  <p align="center">
    A modular ESP32-C6 based room light that syncs brightness via ESP-NOW
    <br />
    <a href="#">See my post on my Website</a>
    &middot;
    <a href="#">Project video on Youtube</a>
  </p>
</div>



<!-- TABLE OF CONTENTS -->
<details>
  <summary>Table of Contents</summary>
  <ol>
    <li>
      <a href="#files">File Structure</a>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
    <li><a href="#license">License</a></li>
  </ol>
</details>



<!-- ABOUT THE PROJECT -->
## About The Project

My dorm room lights are terrible in college, so I decided to make my own. LinkLite is the idea of a modular lighting system that lives locally and not in the cloud while allowing for wireless control and syncing. 

This is the second version of LinkLite, completely redesigned. This time with a custom PCB and much increased brightness.

### Built With

These are the major libraries I used in the programming in the project:

* RotaryEncoder
* Preferences
* ESP-NOW

<!-- CONTACT -->
## Files

The PCB project files can be found under ```kicad/linklite_v2```

```
linklite-v2
├── data
├── kicad
│   └── linklite_v2
├── linklite-bar
├── linklite-controller
├── temperature_logger
├── README.md
├── CLAUDE.md
└── LICENSE.txt
```

The arduino sketches for the ```linklite-bar```, ```linklite-controller```, and ```temperature_logger``` can be found under their respective folders, and my data collected can be found in the ```data``` folder along with the MATLAB script for plot generation.


<!-- CONTACT -->
## Contact

Logan Fick -  loganfickcontact@gmail.com

Project Link: [https://github.com/weasalcrafter/linklite-v2](https://github.com/weasalcrafter/linklite-v2)

Website: [https://loganfick.com/projects/linklite-v2](https://loganfick.com/projects/linklite-v2)

<!-- ACKNOWLEDGMENTS -->
## Acknowledgments
These are the tools I used for this project for designing the schematic, PCB layout, and programming.


* [Claude Code](claude.com)
* [KiCad 10.0](https://www.kicad.org/)
* [Digikey Tools](https://www.digikey.com/en/resources/online-conversion-calculators)
* [MATLAB](https://www.mathworks.com/products/matlab.html)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE.TXT) file for details.

