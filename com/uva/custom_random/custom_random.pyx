from sets import Set

cdef extern from "mcmc/random.h" namespace "mcmc::Random":
    cdef cppclass Random:
        Random(int seed) except +
        Random(int seed_hi, int seed_lo) except +
        # long seed(int x)      # added to later version of mcmc::Random
        # long rand()           # added to later version of mcmc::Random
        long randint(long start, long upto)
        float random()
        float randn()
        float gamma(float p1, float p2)

cdef class CustomRandom:
    cdef Random *thisptr
    def __cinit__(self, int seed):
        self.thisptr = new Random(seed)
    def __dealloc_(self):
        del self.thisptr
    # def seed(self, field):
    #     return self.thisptr.seed(field)

    # def rand(self):
    #     return self.thisptr.rand()
    def randint(self, start, upto):
        return self.thisptr.randint(start, upto)
    def random(self):
        return self.thisptr.random()

    def randn(self, *args, **kwargs):
        return getattr(self, "_randn" + str(len(args)))(*args, **kwargs)
    def _randn0(self):
        return self.thisptr.randn()
    def _randn1(self, K):
        a = []
        for x in xrange(0, K):
            a.append(self._randn0())
        return a
    def _randn2(self, K, N):
        a = []
        for x in xrange(0, K):
            a.append(self._randn1(N))
        return a

    def gamma(self, *args, **kwargs):
        return getattr(self, "_gamma" + str(len(args)))(*args, **kwargs)
    def _gamma2(self, p1, p2):
        return self.thisptr.gamma(p1, p2)
    def _gamma3(self, p1, p2, n):
        a = []
        for x in xrange(0, n):
            a.append(self._gamma2(p1, p2))
        return a
    def _gamma4(self, p1, p2, n1, n2):
        a = []
        for x in xrange(0, n1):
            a.append(self._gamma3(p1, p2, n2))
        return a

    def sample(self, *args, **kwargs):
        return getattr(self, "_sample" + str(len(args)))(*args, **kwargs)
    def _sample3(self, start, upto, count):
        accu = Set()
        while count > 0:
            r = self.randint(start, upto)
            if not r in accu:
                accu.add(r)
                count -= 1
        return accu
    def _sample2(self, population, count):
        # result = polulation.__class__.__new__(population.__class__)
        accu = self._sample3(0, len(population), count)
        result = population.__class__();
        i = 0
        # BEWARE! sort the population for order compatibility w/ mcmc::Random
        for p in sorted(population):
            if i in accu:
                result.add(p)
            i += 1
        return result
