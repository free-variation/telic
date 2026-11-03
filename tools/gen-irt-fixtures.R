# Regenerate the IRT fixtures from the datasets bundled with R's mirt package,
# which tests/lib/159_irt.telic fits and checks against mirt's own estimates:
#   data/lsat7.tsv    LSAT7, 1,000 people x 5 right/wrong items (Bock and
#                     Lieberman, 1970), expanded from mirt's frequency table
#   data/science.tsv  Science, 392 people x 4 items on a four-point scale
#                     (Karlheinz and Melich, 1992)
#   data/irt-covariates.tsv
#                     simulated: 1,000 people x 10 right/wrong 2PL items and
#                     two covariates x (normal) and g (0/1), the trait
#                     0.6x + 0.5g plus a standard normal residual
#
#   Rscript tools/gen-irt-fixtures.R
suppressMessages(library(mirt))
write.table(expand.table(LSAT7), "data/lsat7.tsv", sep = "\t", quote = FALSE, row.names = FALSE)
write.table(Science, "data/science.tsv", sep = "\t", quote = FALSE, row.names = FALSE)

set.seed(20261102)
n_people <- 1000
n_items <- 10
x <- round(rnorm(n_people), 3)
g <- rbinom(n_people, 1, 0.5)
trait <- 0.6 * x + 0.5 * g + rnorm(n_people)
slopes <- runif(n_items, 0.8, 1.8)
intercepts <- runif(n_items, -1.5, 1.5)
responses <- sapply(1:n_items, function(j) as.integer(runif(n_people) < plogis(slopes[j] * trait + intercepts[j])))
colnames(responses) <- paste0("item", 1:n_items)
write.table(data.frame(responses, x = x, g = g), "data/irt-covariates.tsv", sep = "\t", quote = FALSE, row.names = FALSE)

# the latent regression's full-information coefficient standard errors for
# tests/lib/159_irt.telic: the central-difference Hessian of the marginal
# log-likelihood at mirt's estimates over every parameter (slopes, intercepts,
# coefficients and, under Rasch, the residual variance), on mirt's 61 evenly
# spaced nodes over [-6, 6]; mirt's own SE.type = "central" reports other
# values for x (0.0374 2PL, 0.0652 Rasch)
covariates <- cbind(x = x, g = g)
nodes <- seq(-6, 6, length = 61)
marginal <- function(slopes, intercepts, coefficients, variance) {
  probabilities <- plogis(outer(nodes, slopes) + matrix(intercepts, 61, n_items, byrow = TRUE))
  likelihood <- exp(responses %*% t(log(probabilities)) + (1 - responses) %*% t(log(1 - probabilities)))
  prior <- exp(-0.5 * outer(as.vector(covariates %*% coefficients), nodes, "-")^2 / variance)
  sum(log(rowSums(likelihood * prior / rowSums(prior))))
}
hessian <- function(f, p) {
  n <- length(p); h <- matrix(0, n, n); step <- pmax(abs(p), 1) * 1e-4
  for (i in 1:n) for (j in 1:i) {
    ei <- replace(rep(0, n), i, step[i]); ej <- replace(rep(0, n), j, step[j])
    h[i, j] <- h[j, i] <- (f(p + ei + ej) - f(p + ei - ej) - f(p - ei + ej) + f(p - ei - ej)) / (4 * step[i] * step[j])
  }
  h
}
two_parameter <- coef(mirt(responses, 1, "2PL", covdata = data.frame(covariates), formula = ~ x + g, verbose = FALSE), simplify = TRUE)
p <- c(two_parameter$items[, "a1"], two_parameter$items[, "d"], two_parameter$lr.betas[2:3, 1])
h <- hessian(function(q) marginal(q[1:10], q[11:20], q[21:22], 1), p)
cat("2PL coefficient errors", sqrt(diag(solve(-h)))[21:22], "\n")
rasch <- coef(mirt(responses, 1, "Rasch", covdata = data.frame(covariates), formula = ~ x + g, verbose = FALSE), simplify = TRUE)
p <- c(rasch$items[, "d"], rasch$lr.betas[2:3, 1], rasch$cov[1, 1])
h <- hessian(function(q) marginal(rep(1, 10), q[1:10], q[11:12], q[13]), p)
cat("Rasch coefficient errors", sqrt(diag(solve(-h)))[11:12], "\n")

# data/irt-bifactor.tsv  simulated: 2,000 people x 12 right/wrong bifactor 2PL items, items 1-4 on
#                        the general trait only, items 5-8 also on specific factor 1 and items 9-12
#                        on specific factor 2; mirt's bfactor estimates (21 quadrature points per
#                        dimension) are printed for tests/lib/159_irt.telic
set.seed(20261103)
n_people <- 2000
general <- rnorm(n_people)
specific <- matrix(rnorm(n_people * 2), n_people, 2)
general_slopes <- c(1.2, 0.9, 1.5, 1.0, 1.3, 0.8, 1.1, 1.4, 1.0, 1.2, 0.9, 1.6)
specific_slopes <- c(0, 0, 0, 0, 0.9, 0.7, 1.0, 0.6, 0.8, 1.1, 0.5, 0.9)
groups <- c(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2)
intercepts <- c(0.5, -0.3, 1.0, 0.0, -0.8, 0.4, 0.2, -0.5, 1.2, -1.0, 0.6, 0.1)
responses <- sapply(1:12, function(j) {
  s <- if (groups[j] == 0) 0 else specific[, groups[j]]
  as.integer(runif(n_people) < plogis(general_slopes[j] * general + specific_slopes[j] * s + intercepts[j]))
})
colnames(responses) <- paste0("item", 1:12)
write.table(responses, "data/irt-bifactor.tsv", sep = "\t", quote = FALSE, row.names = FALSE)
fit <- bfactor(responses, ifelse(groups == 0, NA, groups), quadpts = 21, verbose = FALSE)
print(round(coef(fit, simplify = TRUE)$items[, c("a1", "a2", "a3", "d")], 4))
cat("log-likelihood", round(extract.mirt(fit, "logLik"), 3), "\n")
