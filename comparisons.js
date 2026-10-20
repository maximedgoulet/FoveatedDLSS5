// The comparisons the page offers. Add an entry per screenshot pair; the
// files go in images/. The first entry opens by default; ?c=<id> opens another.
window.COMPARISONS = [
  {
    id: "witcher3",
    title: "The Witcher 3 — 7680×2160, RTX 4090",
    left: { src: "images/witcher3-off.webp", label: "Foveation off · 49 fps" },
    right: { src: "images/witcher3-on.webp", label: "Foveation on · 81 fps" },
    note: "DLSS 5 NR, 2 passes, 100 % centre 3295×1157, edges 25 % · FPS: ReShade counter, top right",
  },
];
