
class OLED_t;

struct OLED_VCI_T : public uni::VideoControlInterface {
	OLED_t* oled;
	OLED_VCI_T(OLED_t* _oled) : oled(_oled) {}
	virtual void SetCursor(const uni::Point& disp) const;
	virtual uni::Point GetCursor() const { _TODO return uni::Point(0, 0); }
	virtual void DrawPoint(const uni::Point& disp, uni::Color color) const;
	virtual void DrawRectangle(const uni::Rectangle& rect) const;
	virtual void DrawFont(const uni::Point& disp, const uni::DisplayFont& font) const { _TODO }
	virtual uni::Color GetColor(uni::Point p) const;
};

extern OLED_VCI_T oled_vci_t;

typedef byte OLED_PAGE_t[8][128];

// IIC Driven
class OLED_t {
protected:
	uni::IIC_t& iic;
	byte ADDR;
	OLED_PAGE_t* page = NULL;

public:
	OLED_t(byte _addr, uni::IIC_t& _iic) : ADDR(_addr), iic(_iic) {}
	friend struct OLED_VCI_T;
protected:
	void Send(byte addr, byte data) {
		iic.SendStart();
		iic << ADDR;
		iic << addr;
		iic << data;
		iic.SendStop();
	}
	
	void SendCmd(byte cmd) {
		Send(0x00, cmd);
	}
	void SendDat(byte dat) {
		Send(0x40, dat);
	}
public:
	void setOutput();

	void setPage(OLED_PAGE_t* _page) {
		page = _page;
	}

	void Fill(byte data) {
		for0 (m, 8) {
			SendCmd(0xb0 + m);
			SendCmd(0x00);
			SendCmd(0x10);
			for0(n, 128) SendDat(data);
		}
	}

	void Clear() {
		Fill(0x00);
	}

	void enAble(bool ena) {
		if (ena) {
			SendCmd(0xAF);
			SendCmd(0x8D);
			SendCmd(0x14);
		}
		else {
			SendCmd(0xAE);
			SendCmd(0x8D);
			SendCmd(0x10);
		}
	}

	OLED_VCI_T getControlInterface() {
	    return OLED_VCI_T(this);
	}
	uni::VideoControlBlock getControlBlock();

	void Refresh() {
		if (page) for0(m, 8) {
			SendCmd(0xb0 + m);
			SendCmd(0x00);
			SendCmd(0x10);
			for0(n, 128) SendDat((*page)[m][n]);
		}
	}

};



