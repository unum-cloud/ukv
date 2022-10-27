
import com.unum.ukv.DataBaseUMemKV;
import org.junit.Test;

import java.util.Arrays;

public class DataBaseUMemKVTest {
    static {
        DataBaseUMemKV.init();
    }
    @Test
    public void test() {
        DataBaseUMemKV.Context ctx = new DataBaseUMemKV.Context("");
        ctx.put(42, "hey".getBytes());
        assert Arrays.equals(ctx.get(42), "hey".getBytes()) : "Received wrong value";

        DataBaseUMemKV.Transaction txn = ctx.transaction();
        txn.put("any", 42, "meaning of life".getBytes());
        assert Arrays.equals(txn.get("any", 42), "meaning of life".getBytes()) : "Wrong philosophy";
        txn.commit();
        assert Arrays.equals(ctx.get("any", 42), "meaning of life".getBytes()) : "Accepted wrong philosophy";

        ctx.close();
        System.out.println("Success!");
    }
}
