require('dotenv').config();
const { GoogleGenerativeAI } = require('@google/generative-ai');

async function testGemini() {
    const key = process.env.GEMINI_API_KEY;
    if (!key) {
        console.error("❌ ERROR: ยังไม่ได้ใส่ GEMINI_API_KEY ในไฟล์ .env");
        process.exit(1);
    }

    console.log("⏳ กำลังทดสอบเชื่อมต่อกับ Google Gemini API...");
    
    try {
        const genAI = new GoogleGenerativeAI(key);
        const model = genAI.getGenerativeModel({ model: "gemini-2.5-flash" });
        const result = await model.generateContent("สวัสดีครับ! แนะนำตัวสั้นๆ เป็นภาษาไทย 1 ประโยคครับ");
        
        console.log("✅ ยอดเยี่ยม! ทดสอบผ่านแล้ว! Gemini ตอบกลับมาว่า:");
        console.log("🤖 Gemini: " + result.response.text());
        console.log("\n(API Key ของคุณพร้อมใช้งานและสามารถนำไปรันบนเซิร์ฟเวอร์เว็บได้แล้วครับ)");
    } catch (error) {
        console.error("❌ ล้มเหลว: ไม่สามารถเชื่อมต่อกับ Gemini API ได้ โปรดตรวจสอบ Key ของคุณ");
        console.error(error.message);
    }
}

testGemini();
