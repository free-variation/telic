# Water — loadable library reference

The words below are not in the base image; they are defined by loading a file
from `lib/`. `"lib/statistics.h2o" load` brings in the linear-algebra,
regression, generalized-linear-model, and gradient-boosting words, and
`"plot" load-library` the plotting words. Once a library is loaded its words
answer to `help`, `man`, and `apropos` exactly like built-ins — this file is the
source `gen-help.py` reads for them, alongside `reference.md`.

The statistics library is native-only: it reaches BLAS/LAPACK (and, for the
xgboost words, libxgboost) through the FFI, which the wasm build excludes. The
plotting library is pure forth (no FFI) and works under wasm; only its
`save-figure` / `show-figure` output words use a subprocess and are native-only.

## Linear algebra (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `svd` | `( A -- U S VT )` | Thin singular value decomposition via LAPACKE dgesvd: `A = U diag(S) VT`, with `S` the 1×min(m,n) singular values. Column signs of U/VT are not canonical, so pin goldens on S and the reconstruction, not raw U/VT entries |
| `fit-linear` | `( m y -- beta )` | Ordinary least squares via LAPACKE dgelsd; `m` is observations×predictors (observations ≥ predictors), `y` the observations×1 response, `beta` the predictors×1 coefficients |
| `fit-augmented` | `( augmented -- beta )` | Least squares of an `[X | y]` block whose last column is the response |

## Regression (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `linear-regression` | `( dataset predictors response replications -- summaries )` | OLS with nonparametric bootstrap inference: a `{ :estimate :se :bias :ci-low :ci-high }` frame per coefficient over `replications` refits |
| `fit-logistic` | `( X y max-iterations tolerance -- beta )` | Binary logistic regression by Firth-penalized IRLS (estimates stay finite under separation); `X` includes the intercept column, `y` in {0,1} |
| `fit-logistic-ridge` | `( X y max-iterations tolerance lambda -- beta )` | L2-penalized logistic by IRLS; `lambda` penalizes ‖beta‖²/2 with the intercept column unpenalized, `lambda` 0 the plain MLE (no Firth). Each step appends sqrt(`lambda`)·I rows to the weighted design and solves that least-squares system by Householder QR (`dgels`); a design `dgels` reports rank-deficient falls back to `dgelsd`, which answers a minimum-norm solution. The normal equations are not used: squaring the design costs about half the digits, which floors the beta step near 1e-6 at small `lambda` and prevents convergence at a 1e-8 tolerance |
| `fit-augmented-logistic` | `( augmented -- beta )` | Firth logistic fit of an `[X | y]` block |
| `logistic-regression` | `( dataset predictors response replications -- summaries )` | Firth logistic with the bootstrap per-coefficient summaries of `linear-regression` |
| `cv-logistic-ridge` | `( X y units lambdas n-folds -- fr )` | k-fold cross-validation of ridge logistic over a `lambdas` grid, returning `{ :lambdas :deviances :best }`; `X` excludes the intercept (added internally, unpenalized), `units` index rows so per-cluster index arrays give cluster CV |
| `pcv-logistic-ridge` | `( X y units lambdas n-folds -- fr )` | `cv-logistic-ridge` with the (lambda, fold) cells evaluated under `pmap`; results are identical, the cells being deterministic |

## Generalized linear models (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `fit-glm` | `( X y family max-iterations tolerance -- beta )` | IRLS for a family object — a frame of three stack quotations `:inverse-link ( eta -- mu )`, `:mean-derivative ( eta -- dmu/deta )`, `:variance ( mu -- V )`. Each step solves a weighted least squares via `fit-linear`. Provided families: `gaussian-identity`, `poisson-log`, `gamma-log`, `binomial-logit` |
| `fit-gamma` | `( X y max-iterations tolerance -- beta )` | Gamma regression, log link — `fit-glm` with `gamma-log` |
| `fit-poisson` | `( X y max-iterations tolerance -- beta )` | Poisson regression, log link — `fit-glm` with `poisson-log` |
| `negative-binomial-log` | `( theta -- family )` | The NB2 family at a known dispersion: log link, variance μ + μ²/θ, so a large θ approaches `poisson-log`. Pass to `fit-glm` to fit coefficients with θ held fixed; `theta` is curried into the variance quotation, costing one small anonymous word per call |
| `negative-binomial-theta` | `( y mu -- theta )` | Maximum-likelihood NB2 dispersion at fitted means, by golden-section search on ln θ over the moment estimate widened a factor of 1000 each way. When the residual variance does not exceed the mean the likelihood rises monotonically in θ and the answer is the top of that window, which reports that Poisson fits rather than a dispersion |
| `fit-negative-binomial` | `( X y max-iterations tolerance -- beta theta )` | Negative binomial (NB2) regression, log link, estimating the dispersion: starts from the Poisson fit, then alternates `negative-binomial-theta` at the current means with an IRLS `beta` at that θ. Answers both. `max-iterations` and `tolerance` bound both loops; θ cannot be pinned tighter than `beta` is, so the alternation also stops once a round no longer shrinks the change in θ |
| `fit-multinomial` | `( X y reference-class max-iterations tolerance -- beta )` | Multinomial (softmax) logistic by Newton–Raphson, baseline-category parametrization with `reference-class` as the baseline; `y` holds integer labels 0..K−1, `beta` is predictors×(K−1), one coefficient column per non-reference class. As the plain MLE it diverges under separation |
| `fit-multinomial-ridge` | `( X y reference-class max-iterations tolerance lambda -- beta )` | `fit-multinomial` with an L2 penalty λ·‖β‖²/2 on every coefficient except each class's intercept; `lambda` 0 is the plain MLE (what `fit-multinomial` calls), `lambda` > 0 keeps the estimate finite under separation |
| `predict-multinomial` | `( beta X reference-class -- probabilities )` | Softmax probabilities from a `fit-multinomial`/`fit-multinomial-ridge` model: n×K, columns in label order 0..K−1 (the `reference-class` column is `1/Σ` weights). Each row sums to 1 |

## Gradient boosting (lib/statistics.h2o)

| Word | Stack effect | Summary |
| --- | --- | --- |
| `fit-xgb` | `( X y fit-params -- booster )` | Train an XGBoost booster on features `X` (n×k) and response `y` (n×1); `fit-params` are keyed by xgboost parameter name, `:rounds` drives the boosting loop (default 100). Native-only (libxgboost via FFI) |
| `xgb-predict` | `( booster X -- predictions )` | n×1 scores for an n×k feature matrix; the booster is not freed |
| `xgb-free` | `( booster -- )` | Free a booster handle |
| `xgb-importance` | `( booster importance-type -- scores )` | k×1 per-feature importance, row i is feature i; `importance-type` is `"gain"`, `"weight"`, `"cover"`, `"total_gain"`, or `"total_cover"`. Rank with `matrix>array argsort reverse` |

## Plotting (lib/plot.h2o)

A figure accumulates marks (data- or pixel-space coordinates plus the style in
effect); nothing maps to pixels until `figure>svg` resolves the domain and
renders, so draw order is free and the domain may be set after drawing. Style
comes from named `aes` keys, set globally with `aes!` or per figure with
`figure!`.

| Word | Stack effect | Summary |
| --- | --- | --- |
| `figure` | `( width height -- )` | Start a new figure of that canvas size, marks empty, styled from the current `aes`; becomes the current figure |
| `aes!` | `( fr -- )` | Merge a frame of style keys into the global `aes` defaults, for figures created afterward |
| `figure!` | `( val sym -- )` | Set a property of the current figure — any `aes` key, or a domain bound (`:xmin`/`:xmax`/`:ymin`/`:ymax`) |
| `figure@` | `( sym -- val )` | Read a property of the current figure |
| `stroke` | `( color -- )` | Set the current stroke color |
| `fill` | `( color -- )` | Set the current fill color |
| `stroke-width` | `( w -- )` | Set the current stroke width |
| `text-anchor` | `( anchor -- )` | Set the current text anchor: `"start"`, `"middle"`, or `"end"` |
| `axes` | `( -- )` | Plot-area border plus labeled x and y ticks, laid out at render time; tick labels use the aes `:tick-format` string (default `{0:g}` — whole numbers as integers, no forced scientific notation); an axis marked categorical (`:x-categorical`/`:y-categorical`, set by `barchart`/`y-categories`) shows no numeric ticks |
| `panel` | `( -- )` | `axes`' themed twin: a filled ground (`:panel-fill`) with gridlines as negative space and labeled ticks, no border; call before the data so it draws underneath; honors the categorical flags like `axes` |
| `x-label` | `( s -- )` | x-axis title, centered below the tick labels |
| `y-label` | `( s -- )` | y-axis title, rotated, centered beside the tick labels |
| `legend` | `( labels colors -- )` | Color key in a strip reserved to the right of the plot area (the plot narrows to fit); one row per label with a filled swatch, `labels`/`colors` equal-length parallel arrays; pixel-space, label text in `:ink` |
| `y-categories` | `( labels -- )` | Place category names along the y axis at positions 1..n (right-anchored, just left of the axis) in place of numeric y-ticks; pins the y domain to `[0.5, n+0.5]` and sets `:y-categorical`, so `axes`/`panel` drop the numeric y-ticks |
| `data-domain` | `( xs ys -- )` | Pin the domain to the padded extents of two data vectors |
| `scatter` | `( xs ys -- )` | Scatter markers at the point pairs; aes `:point-fill` `:point-stroke` `:point-radius`; errors on unequal lengths |
| `series` | `( xs ys -- )` | Connected line through the points in order, split at NaN gaps; aes `:series-stroke` `:series-width` |
| `intervals` | `( lows highs positions -- )` | A horizontal segment from each `low` to its `high` at the y `position`, one 2-point series per interval; equal-length inputs; aes `:series-stroke` `:series-width` |
| `abline` | `( slope intercept -- )` | The line y = slope·x + intercept, clipped to the domain at render; aes `:line-stroke` `:line-width` |
| `histogram` | `( data n-bins -- )` | Equal-width bin-count bars over `data`; pins the domain; aes `:bar-fill` `:bar-stroke` |
| `boxplot` | `( data -- )` | One Tukey boxplot centered in the figure (whiskers to 1.5·IQR, outliers as circles); pins the domain; aes `:box-stroke` |
| `boxplots` | `( arrays labels -- )` | Side-by-side boxplots on a shared y axis, one per vector; `labels` a parallel array of category strings; pins the domain |
| `barchart` | `( heights labels -- )` | Vertical bars on a categorical x axis, one per height from the y=0 baseline; `labels` a parallel array of category strings; pins the domain and sets `:x-categorical` so `axes`/`panel` drop the numeric x-ticks; aes `:bar-fill` `:bar-stroke` |
| `count-barchart` | `( values -- )` | Frequency bars — one per distinct value, height its number of occurrences, most frequent first; labels the values rendered as strings; aes `:bar-fill` `:bar-stroke` |
| `stacked-barchart` | `( matrix labels colors -- )` | Stacked vertical bars: row i is category i (a bar at x=i+1), column j is series j (a segment colored `colors[j]`), stacked from the y=0 baseline; `labels` names the categories under the bars, `colors` is the per-series palette (pass the same array to `legend`); pins the domain and sets `:x-categorical` so `axes`/`panel` drop the numeric x-ticks; aes `:bar-stroke` |
| `annotate` | `( x y label -- )` | Text at data point `(x, y)`, current font size and `text-anchor` |
| `rect-at` | `( x1 y1 x2 y2 -- )` | Rectangle between two data-space corners, mapped through the domain; current `:fill` `:stroke` `:stroke-width` (the data-space analog of `svg-rect`) |
| `svg-line` | `( x1 y1 x2 y2 -- )` | Line segment in pixel coordinates, current stroke |
| `svg-rect` | `( x y w h -- )` | Rectangle at pixel `(x, y)` of size `w`×`h`, current stroke and fill |
| `svg-circle` | `( cx cy r -- )` | Circle of pixel radius `r`, current fill and stroke |
| `svg-text` | `( x y label -- )` | Text at pixel `(x, y)`, current font size and `text-anchor` |
| `figure>svg` | `( -- svg )` | Resolve the domain and render the current figure to an SVG document string |
| `save-figure` | `( name -- )` | Render the current figure as the next version `images-<name>/N.svg` (creating the directory); native-only (`mkdir` via a subprocess) |
| `show-figure` | `( name -- )` | Open a versioned carousel viewer for `images-<name>/` in a browser app window; saves a first version when empty, and later `save-figure`s appear in the carousel; native-only |
| `view-figure` | `( name -- )` | Open the versioned carousel viewer for `images-<name>/` without saving a new version (writing the viewer page if absent); figures must already have been saved there; native-only |
| `scatter-plot` | `( xs ys -- svg )` | Complete scatter plot in one call — 640×480 figure, `axes`, `scatter`, rendered |
| `series-plot` | `( xs ys -- svg )` | Complete line plot of `ys` against `xs`, rendered |
| `histogram-plot` | `( data n-bins -- svg )` | Complete histogram, rendered |
| `boxplot-plot` | `( data -- svg )` | Complete single boxplot, rendered |
| `boxplots-plot` | `( arrays labels -- svg )` | Complete side-by-side boxplots, rendered |
| `barchart-plot` | `( heights labels -- svg )` | Complete bar chart (border and y-ticks, no x-ticks), rendered |
| `count-barchart-plot` | `( values -- svg )` | Complete frequency bar chart from raw values, rendered |
| `plot-tree` | `( tree -- svg )` | Render a `fit-tree` as a node-link diagram — internal nodes show the split, leaves the prediction, edges to the left (condition-true) child then the right |
