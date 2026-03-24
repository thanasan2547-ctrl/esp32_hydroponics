const express = require('express');
const router = express.Router();
const { GoogleGenerativeAI } = require('@google/generative-ai');

router.post('/', async (req, res) => {
    try {
        const { message, context } = req.body;
        if (!message) return res.status(400).json({ error: 'Message is required' });

        const apiKey = process.env.GEMINI_API_KEY;
        if (!apiKey) {
            return res.status(500).json({ error: 'GEMINI_API_KEY is not configured on the server.' });
        }

        const genAI = new GoogleGenerativeAI(apiKey);
        
        // System prompt instructs the AI on its persona and provides current context
        let systemInstruction = `You are a helpful AI assistant for a smart Tomato Hydroponics farm. 
Answer concisely and politely in Thai. Provide practical agricultural advice. 
If the user asks about the current status, refer to the provided sensor data.`;

        if (context) {
            systemInstruction += `\n\n[CURRENT SENSOR DATA]:
- EC (Nutrient Level): ${context.ec || 'Unknown'} mS/cm
- pH (Acidity): ${context.ph || 'Unknown'}
- Water Tank Level: ${context.water || 'Unknown'}%`;
        }

        const model = genAI.getGenerativeModel({ 
            model: "gemini-2.5-flash", 
            systemInstruction: systemInstruction
        });

        const result = await model.generateContent(message);
        const responseText = result.response.text();

        res.json({ reply: responseText });
    } catch (error) {
        console.error('[Chat API] Error:', error);
        res.status(500).json({ error: 'Failed to generate response from AI.' });
    }
});

module.exports = router;
