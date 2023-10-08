import numpy as np
import matplotlib.pyplot as plt

# This code illustrates the rounding issues when doing calendar math
# Since all timestamps are in seconds (since say Jan 1 1970 for Unix)
# how does one compute the year, given a number of days?
# Naively, you could use Days/365.25, which is approximately correct
#

# set up a series of years since 1970
years = np.arange(1970,2100)
days = np.arange(0,365,1)

# the exact number of days (starting with 0) for Jan 1 of a given year
D = (years - 1970)*365 + np.floor((years-1969)/4)

# so D+1, not D is the number of days for a given date
# this equation is transcendental - you could naively invert and obtain:

Y_invert = 1970 + (4*(D+1)-1)/(4*365+1)

# which is close to (D+1)/365.25
# How did we do? Let's see
misses=[] # should be zero
for cd in days:
	D = (years - 1970)*365 + np.floor((years-1969)/4)+cd
	Y_invert = np.array(1970 + (4*(D+1)-1)/(4*365+1),dtype=int)
	misses.append(np.sum(np.abs(Y_invert-years)))
	
# special check of last day of leap years
LO = 1970%4 # leap offset
D = (years[LO::4] - 1970)*365 + np.floor((years[LO::4]-1969)/4)+365
Y_invert = np.array(1970 + (4*(D+1)-1)/(4*365+1),dtype=int)
misses.append(np.sum(np.abs(Y_invert-years[LO::4])))

plt.plot(np.append(days,[365]),misses)
plt.show()

# Oops! we see that D+1 doesn't work for Leap Years...
# Going back to D (not D+1) on leap years fixes the problem...
#
# But, how do we know we are truly in a leap year?? 
#
# If we try to compute the implied day in year
#
for cd in [0,1,10,50,300,363,364]:
	D = (years - 1970)*365 + np.floor((years-1969)/4)+cd
	Y_invert = np.array(1970 + (4*(D+1)-1)/(4*365+1),dtype=int)
	DIY = D-((Y_invert - 1970)*365 + np.floor((Y_invert-1969)/4))
	plt.plot(years,DIY,label=str(cd))

# add last day of leap years as check
D = (years[LO::4] - 1970)*365 + np.floor((years[LO::4]-1969)/4)+365
Y_invert = np.array(1970 + (4*(D+1)-1)/(4*365+1),dtype=int)
DIY = D-((Y_invert - 1970)*365 + np.floor((Y_invert-1969)/4))
plt.plot(years[LO::4],DIY,label="365 (D+1)",marker='x')

# Recompute with D, not D+1
D = (years[LO::4] - 1970)*365 + np.floor((years[LO::4]-1969)/4)+365
Y_invert = np.array(1970 + (4*(D+1)-2)/(4*365+1),dtype=int)
DIY = D-((Y_invert - 1970)*365 + np.floor((Y_invert-1969)/4))
plt.plot(years[LO::4],DIY,label="365 (D)",marker='o')

plt.legend()
plt.show()

# We see the recomputed day in a leap year for Day 365 is -1 - that is
# easy to spot fortunately!
# But why?
# Looks like Day 365 would appear to be the next year from a leap year
# So in the computation of the day offset, we would add the leap day to the 
# year we think we are in now (the year just past a leap year)
# But if we are actually in the previous leap year, Day 365 includes the 
# leap day. So we are off by 1!
#
# We could fix this a number of ways, but the way that makes the most sense
# is to recompute the year if the implied day is -1 using one less leap
# day (so 4*(D+1)-2 in numerator)
# Alternatively just set the year to the previous year (which saves
# compute but isn't as clear why
#
