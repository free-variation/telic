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
