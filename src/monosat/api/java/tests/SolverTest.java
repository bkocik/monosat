import org.junit.Test;
import static org.junit.Assert.*;

public class SolverTest {
    @Test
    public void testSolve(){
        Solver s = new Solver();
        assertEquals(s.solve(),true);
        assertEquals(s.solve(),true);
        assertEquals(s.solve(s.True()),true);
    }
}