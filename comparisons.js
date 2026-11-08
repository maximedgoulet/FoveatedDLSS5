// The comparisons the page offers. Add an entry per screenshot pair; the
// files go in images/. The first entry opens by default; ?c=<id> opens another.
window.COMPARISONS = [
  {
    id: "forza",
    title: "Forza Horizon (v0.4): foveation off vs on, 7680×2160",
    left: { src: "images/forza-nr-full.webp", label: "Foveation off · 15 fps" },
    right: { src: "images/forza-foveated.webp", label: "Foveation on · 25 fps" },
    note: "DLSS 5 NR, 2 passes · centre 46×54 %, contour 23×30 % at 0.44, vertical 58 % · NR on 42 % of the pixels · overlay open with the zones drawn · rendered FPS (frame generation doubles the counter)",
  },
  {
    id: "forza-nr",
    title: "Forza Horizon: game image vs DLSS 5 NR, 7680×2160",
    left: { src: "images/forza-nr-off.webp", label: "NR off · 55 fps" },
    right: { src: "images/forza-nr-full.webp", label: "NR on, full screen · 15 fps" },
    note: "What DLSS 5 NR does to the image, and what it costs at full screen: 2 passes, about 48 ms per frame",
  },
  {
    id: "witcher3",
    title: "The Witcher 3 (v0.1): foveation off vs on, 7680×2160, RTX 4090",
    left: { src: "images/witcher3-off.webp", label: "Foveation off · 49 fps" },
    right: { src: "images/witcher3-on.webp", label: "Foveation on · 81 fps" },
    note: "DLSS 5 NR, 2 passes, 100 % centre 3295×1157, edges 25 % · FPS: ReShade counter, top right",
  },
];
